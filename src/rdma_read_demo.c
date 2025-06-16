// rdma_read_demo.c
// rdma read demo: 支持 IB、RoCE、iWARP，客户端通过 RDMA Read 读取服务端内存，服务端收到 ack 后统计次数。
// 用法：
// 服务器：./rdma_read_demo -s -a <本机IP> -p <端口> [-n <次数>]
// 客户端：./rdma_read_demo -c -a <服务器IP> -p <端口> [-n <次数>]
//
// 依赖：libibverbs, librdmacm
//
// RDMA 基本概念和接口说明见 README.md

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#define MSG_BASE        "你好，汉为信息"
#define MSG_SIZE        64
#define DEFAULT_PORT    18515
#define DEFAULT_COUNT   10

#define ROLE_UNDEF      0
#define ROLE_SERVER     1
#define ROLE_CLIENT     2

struct read_config {
    int         role;
    char        ip[64];
    int         port;
    int         count;
};

void print_usage(const char *prog) {
    printf("用法: %s -s|-c -a <IP> -p <端口> [-n <次数>]\n", prog);
    printf("  -s           以服务端模式启动\n");
    printf("  -c           以客户端模式启动\n");
    printf("  -a <IP>      指定对端IP地址\n");
    printf("  -p <端口>    指定端口 (默认%d)\n", DEFAULT_PORT);
    printf("  -n <次数>    发送/接收消息次数 (默认%d)\n", DEFAULT_COUNT);
}

int parse_args(int argc, char **argv, struct read_config *cfg) {
    int opt;

    memset(cfg, 0, sizeof(*cfg));
    cfg->port  = DEFAULT_PORT;
    cfg->count = DEFAULT_COUNT;
    while ((opt = getopt(argc, argv, "sca:p:n:")) != -1) {
        switch (opt) {
            case 's': cfg->role = ROLE_SERVER; break;
            case 'c': cfg->role = ROLE_CLIENT; break;
            case 'a': strncpy(cfg->ip, optarg, sizeof(cfg->ip)-1); break;
            case 'p': cfg->port = atoi(optarg); break;
            case 'n': cfg->count = atoi(optarg); break;
            default: print_usage(argv[0]); return -1;
        }
    }
    if (cfg->role == ROLE_UNDEF || cfg->ip[0] == '\0') {
        print_usage(argv[0]);
        return -1;
    }
    return 0;
}

// =================== rdma_cm 方式实现 ===================
// 资源结构体
struct rdma_connection {
    struct rdma_event_channel *ec;
    struct rdma_cm_id         *cm_id;
    struct ibv_pd             *pd;
    struct ibv_comp_channel   *comp_ch;
    struct ibv_cq             *cq;
    struct ibv_qp             *qp;
    struct ibv_mr             *mr;
    char                      *buf;
};

struct read_mr_info {
    uint32_t rkey;
    uint64_t vaddr;
};

int rdma_connection_init(struct rdma_connection *conn, struct read_config *cfg) {
    struct sockaddr_in addr;
    int                ret = 0;

    memset(conn, 0, sizeof(*conn));
    conn->ec = rdma_create_event_channel();
    if (!conn->ec) {
        fprintf(stderr, "rdma_create_event_channel 失败\n");
        return -1;
    }

    ret = rdma_create_id(conn->ec, &conn->cm_id, NULL, RDMA_PS_TCP);
    if (ret) {
        fprintf(stderr, "rdma_create_id 失败 %d\n", ret);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(cfg->port);
    addr.sin_addr.s_addr = inet_addr(cfg->ip);
    if (cfg->role == ROLE_SERVER) {
        ret = rdma_bind_addr(conn->cm_id, (struct sockaddr*)&addr);
        if (ret) {
            fprintf(stderr, "rdma_bind_addr 失败 %d\n", ret);
            return -1;
        }
        ret = rdma_listen(conn->cm_id, 1);
        if (ret) {
            fprintf(stderr, "rdma_listen 失败 %d\n", ret);
            return -1;
        }
    } else {
        ret = rdma_resolve_addr(conn->cm_id, NULL, (struct sockaddr*)&addr, 2000);
        if (ret) {
            fprintf(stderr, "rdma_resolve_addr 失败 %d\n", ret);
            return -1;
        }
    }
    return 0;
}

void rdma_connection_cleanup(struct rdma_connection *conn) {
    if (conn->mr)      ibv_dereg_mr(conn->mr);
    if (conn->cq)      ibv_destroy_cq(conn->cq);
    if (conn->comp_ch) ibv_destroy_comp_channel(conn->comp_ch);
    if (conn->pd)      ibv_dealloc_pd(conn->pd);
    if (conn->cm_id)   rdma_destroy_id(conn->cm_id);
    if (conn->ec)      rdma_destroy_event_channel(conn->ec);
    if (conn->buf)     free(conn->buf);
}

int wait_event(struct rdma_connection *conn, enum rdma_cm_event_type expect, struct rdma_cm_event **evt) {
    int ret = 0;

    ret = rdma_get_cm_event(conn->ec, evt);
    if (ret) {
        fprintf(stderr, "rdma_get_cm_event 失败 %d\n", ret);
        return -1;
    }

    if ((*evt)->event != expect) {
        fprintf(stderr, "期望事件 %d, 实际事件 %d\n", expect, (*evt)->event);
        rdma_ack_cm_event(*evt);
        return -1;
    }
    return 0;
}

int build_qp(struct rdma_connection *conn) {
    struct ibv_qp_init_attr init_attr;

    conn->pd = ibv_alloc_pd(conn->cm_id->verbs);
    if (!conn->pd) {
        fprintf(stderr, "ibv_alloc_pd 失败\n");
        return -1;
    }

    conn->comp_ch = ibv_create_comp_channel(conn->cm_id->verbs);
    if (!conn->comp_ch) {
        fprintf(stderr, "ibv_create_comp_channel 失败\n");
        return -1;
    }

    conn->cq = ibv_create_cq(conn->cm_id->verbs, 10, NULL, conn->comp_ch, 0);
    if (!conn->cq) {
        fprintf(stderr, "ibv_create_cq 失败\n");
        return -1;
    }

    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.send_cq          = conn->cq;
    init_attr.recv_cq          = conn->cq;
    init_attr.qp_type          = IBV_QPT_RC;
    init_attr.cap.max_send_wr  = 10;
    init_attr.cap.max_recv_wr  = 10;
    init_attr.cap.max_send_sge = 1;
    init_attr.cap.max_recv_sge = 1;
    int ret = rdma_create_qp(conn->cm_id, conn->pd, &init_attr);
    if (ret) {
        fprintf(stderr, "rdma_create_qp 失败\n");
        return -1;
    }
    conn->qp = conn->cm_id->qp;
    return 0;
}

int reg_mem(struct rdma_connection *conn) {
    if (posix_memalign((void**)&conn->buf, 4096, MSG_SIZE) != 0) {
        fprintf(stderr, "posix_memalign 失败\n");
        return -1;
    }
    memset(conn->buf, 0, MSG_SIZE);

    conn->mr = ibv_reg_mr(conn->pd, conn->buf, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!conn->mr) {
        fprintf(stderr, "ibv_reg_mr 失败\n");
        return -1;
    }
    return 0;
}

int modify_qp_timeout(struct rdma_connection *conn, int time) {
    struct ibv_qp_attr      qp_attr;
    struct ibv_qp_init_attr init_attr;
    
    // 先查询当前 QP 状态
    if (ibv_query_qp(conn->qp, &qp_attr, IBV_QP_STATE, &init_attr) == 0) {
        printf("[服务端] 当前 QP 状态: %d (RTR=2, RTS=3)\n", qp_attr.qp_state);
        
        // 根据 QP 状态设置不同的属性
        if (qp_attr.qp_state >= IBV_QPS_RTR) {
            // RTR 状态或之后可以设置 min_rnr_timer
            memset(&qp_attr, 0, sizeof(qp_attr));
            qp_attr.min_rnr_timer = time;
            if (ibv_modify_qp(conn->qp, &qp_attr, IBV_QP_MIN_RNR_TIMER)) {
                fprintf(stderr, "[服务端] 设置 QP RNR 超时属性失败: %s\n", strerror(errno));
                return -1;
            } 
        }
        
        if (qp_attr.qp_state == IBV_QPS_RTS) {
            // RTS 状态可以设置 timeout
            memset(&qp_attr, 0, sizeof(qp_attr));
            qp_attr.timeout = time;
            if (ibv_modify_qp(conn->qp, &qp_attr, IBV_QP_TIMEOUT)) {
                fprintf(stderr, "[服务端] 设置 QP 超时属性失败: %s\n", strerror(errno));
                return -1;
            }
        }
        return 0;
    }
    return -1;
}

int run_server(struct read_config *cfg) {
    struct rdma_connection server_conn;
    struct rdma_cm_event  *evt = NULL;
    struct rdma_cm_id     *child = NULL;
    struct rdma_conn_param conn_param;
    int                    received_count = 0;
    int                    listen_sock = -1, conn_sock = -1;
    struct read_mr_info    local_info, remote_info;
    int                    sock_opt = 1;
    struct sockaddr_in     sin;

    printf("[服务端] 启动，监听 %s:%d，等待连接...\n", cfg->ip, cfg->port);
    if (rdma_connection_init(&server_conn, cfg)) {
        fprintf(stderr, "初始化会话资源失败\n");
        return -1;
    }
    if (wait_event(&server_conn, RDMA_CM_EVENT_CONNECT_REQUEST, &evt)) {
        fprintf(stderr, "等待连接请求失败\n");
        goto cleanup;
    }
    child = evt->id;
    rdma_ack_cm_event(evt);
    server_conn.cm_id = child;
    
    if (build_qp(&server_conn)) {
        fprintf(stderr, "传输队列创建失败\n");
        goto cleanup;
    }
    if (reg_mem(&server_conn)) {
        fprintf(stderr, "rdma 内存注册失败\n");
        goto cleanup;
    }

    // 初始化内容为"你好，汉为信息1"
    snprintf(server_conn.buf, MSG_SIZE, "%s1", MSG_BASE);
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    conn_param.retry_count = 7;        // 设置重试次数
    conn_param.rnr_retry_count = 7;    // 设置 RNR 重试次数
    if (rdma_accept(server_conn.cm_id, &conn_param)) {
        fprintf(stderr, "rdma_accept 失败\n");
        goto cleanup;
    }
    if (wait_event(&server_conn, RDMA_CM_EVENT_ESTABLISHED, &evt)) {
        fprintf(stderr, "等待连接建立成功事件失败\n");
        goto cleanup;
    }
    rdma_ack_cm_event(evt);

    if (modify_qp_timeout(&server_conn,12)) {
        fprintf(stderr, "设置qp超时时间失败\n");
    }

    // 建立 socket 用于 rkey/vaddr 交换和 ack
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        fprintf(stderr, "socket 创建失败\n");
        goto cleanup;
    }
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &sock_opt, sizeof(sock_opt));
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(cfg->port + 1);
    if (bind(listen_sock, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        fprintf(stderr, "bind 失败\n");
        goto cleanup;
    }
    listen(listen_sock, 1);

    conn_sock = accept(listen_sock, NULL, NULL);
    if (conn_sock < 0) {
        fprintf(stderr, "accept 失败\n");
        goto cleanup;
    }
    local_info.rkey = server_conn.mr->rkey;
    local_info.vaddr = (uintptr_t)server_conn.buf;
    // 发送本地 rkey/vaddr，接收对方 rkey/vaddr（客户端用不到）
    if (write(conn_sock, &local_info, sizeof(local_info)) != sizeof(local_info)) {
        fprintf(stderr, "write local_info 失败\n");
        goto cleanup;
    }
    if (read(conn_sock, &remote_info, sizeof(remote_info)) != sizeof(remote_info)) {
        fprintf(stderr, "read remote_info 失败\n");
        goto cleanup;
    }
    printf("[服务端] 连接建立，等待客户端读取...\n");

    // 轮询等待客户端 ack
    char ack_buf[8];
    while (received_count < cfg->count) {
        int n = read(conn_sock, ack_buf, sizeof(ack_buf));
        if (n > 0) {
            received_count++;
            printf("[服务端] 收到第 %d 次客户端读取 ack\n", received_count);
            // 更新内容为"你好，汉为信息N"
            snprintf(server_conn.buf, MSG_SIZE, "%s%d", MSG_BASE, received_count + 1);
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            fprintf(stderr, "ack 读取失败\n");
            break;
        }
    }
    printf("[服务端] 客户端读取完毕，退出。\n");
cleanup:
    if (conn_sock >= 0) close(conn_sock);
    if (listen_sock >= 0) close(listen_sock);
    rdma_connection_cleanup(&server_conn);
    return 0;
}

int run_client(struct read_config *cfg) {
    struct rdma_connection client_conn;
    struct rdma_cm_event  *evt = NULL;
    struct rdma_conn_param conn_param;
    struct read_mr_info    local_info, remote_info;
    struct ibv_sge         sge;
    struct ibv_send_wr     wr, *bad_wr = NULL;
    int                    sockfd = -1;
    struct sockaddr_in     sin;
    struct ibv_wc          wc;

    printf("[客户端] 启动，连接 %s:%d...\n", cfg->ip, cfg->port);
    if (rdma_connection_init(&client_conn, cfg)) {
        fprintf(stderr, "初始化会话资源失败\n");
        return -1;
    }
    if (wait_event(&client_conn, RDMA_CM_EVENT_ADDR_RESOLVED, &evt)) {
        fprintf(stderr, "地址解析失败\n");
        goto cleanup;
    }
    rdma_ack_cm_event(evt);
    if (rdma_resolve_route(client_conn.cm_id, 2000)) {
        fprintf(stderr, "路由解析失败\n");
        goto cleanup;
    }
    if (wait_event(&client_conn, RDMA_CM_EVENT_ROUTE_RESOLVED, &evt)) {
        fprintf(stderr, "路由解析事件失败\n");
        goto cleanup;
    }
    rdma_ack_cm_event(evt);
    
    if (build_qp(&client_conn)) {
        fprintf(stderr, "传输队列创建失败\n");
        goto cleanup;
    }
    if (reg_mem(&client_conn)) {
        fprintf(stderr, "rdma 内存注册失败\n");
        goto cleanup;
    }
    
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    conn_param.retry_count = 7;        // 设置重试次数
    conn_param.rnr_retry_count = 7;    // 设置 RNR 重试次数
    if (rdma_connect(client_conn.cm_id, &conn_param)) {
        fprintf(stderr, "rdma_connect 失败\n");
        goto cleanup;
    }
    if (wait_event(&client_conn, RDMA_CM_EVENT_ESTABLISHED, &evt)) {
        fprintf(stderr, "等待连接建立成功事件失败\n");
        goto cleanup;
    }
    rdma_ack_cm_event(evt);

    if (modify_qp_timeout(&client_conn,12)) {
        fprintf(stderr, "设置qp超时时间失败\n");
    }

    sleep(1);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "socket 创建失败\n");
        goto cleanup;
    }
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(cfg->ip);
    sin.sin_port = htons(cfg->port + 1);
    if (connect(sockfd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        fprintf(stderr, "connect 失败\n");
        goto cleanup;
    }
    // 先接收服务端 rkey/vaddr，再发送本地 rkey/vaddr（服务端用不到）
    if (read(sockfd, &remote_info, sizeof(remote_info)) != sizeof(remote_info)) {
        fprintf(stderr, "read remote_info 失败\n");
        goto cleanup;
    }
    local_info.rkey = client_conn.mr->rkey;
    local_info.vaddr = (uintptr_t)client_conn.buf;
    if (write(sockfd, &local_info, sizeof(local_info)) != sizeof(local_info)) {
        fprintf(stderr, "write local_info 失败\n");
        goto cleanup;
    }

    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uintptr_t)client_conn.buf;
    sge.length = MSG_SIZE;
    sge.lkey   = client_conn.mr->lkey;
    memset(&wr, 0, sizeof(wr));
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_RDMA_READ;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_info.vaddr;
    wr.wr.rdma.rkey        = remote_info.rkey;
    printf("[客户端] 连接建立，开始 RDMA Read...\n");
    for (int i = 0; i < cfg->count; ++i) {
        if (ibv_post_send(client_conn.qp, &wr, &bad_wr)) {
            fprintf(stderr, "ibv_post_send (RDMA_READ) 失败\n");
            goto cleanup;
        }
        // 等待完成
        while (1) {
            int n = ibv_poll_cq(client_conn.cq, 1, &wc);
            if (n < 0) {
                fprintf(stderr, "ibv_poll_cq 失败\n");
                goto cleanup;
            }
            if (n == 0) {
                continue;
            }
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[客户端] 完成队列错误: %s\n", ibv_wc_status_str(wc.status));
                goto cleanup;
            }
            if (wc.opcode == IBV_WC_RDMA_READ) break;
        }
        // 发送 ack
        if (write(sockfd, "ACK", 3) != 3) {
            fprintf(stderr, "ack 发送失败\n");
            goto cleanup;
        }
        printf("[客户端] 第 %d 次 RDMA Read，内容: %s\n", i+1, client_conn.buf);
        /*rdma速度快，write发送间隔小;服务器read可能未执行到,ack未收到，数据未更新;
        *这种情况下实际客户端rdma read操作完成次数正常；
        */
        usleep(1000);
    }
    printf("[客户端] RDMA Read 完毕，退出。\n");
cleanup:
    if (sockfd >= 0) close(sockfd);
    rdma_connection_cleanup(&client_conn);
    return 0;
}

int main(int argc, char **argv) {
    struct read_config cfg;

    if (parse_args(argc, argv, &cfg)) {
        fprintf(stderr, "参数解析失败\n");
        return -1;
    }
    if (cfg.role == ROLE_SERVER) {
        return run_server(&cfg);
    } else if (cfg.role == ROLE_CLIENT) {
        return run_client(&cfg);
    } else {
        fprintf(stderr, "角色错误\n");
        return -1;
    }
}
