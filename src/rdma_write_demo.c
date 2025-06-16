// rdma_write_demo.c
// rdma_write_demo: 支持 IB、RoCE、iWARP，客户端发送"你好，汉为信息"，服务端收到后打印。
// 用法：
// 服务器：./rdma_write_demo -s -a <本机IP> -p <端口> [-n <次数>]
// 客户端：./rdma_write_demo -c -a <服务器IP> -p <端口> [-n <次数>]
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


#define MSG_STR         "你好，汉为信息"
#define MSG_SIZE        64
#define DEFAULT_PORT    18515
#define DEFAULT_COUNT   10

// 角色定义
#define ROLE_UNDEF      0
#define ROLE_SERVER     1
#define ROLE_CLIENT     2

// 参数结构体
struct write_config {
    int         role;           // 角色：服务端/客户端
    char        ip[64];         // IP地址
    int         port;           // 端口
    int         count;          // 消息收发次数
};

// 打印用法
void print_usage(const char *prog) {
    printf("用法: %s -s|-c -a <IP> -p <端口> [-n <次数>]\n", prog);
    printf("  -s           以服务端模式启动\n");
    printf("  -c           以客户端模式启动\n");
    printf("  -a <IP>      指定对端IP地址\n");
    printf("  -p <端口>    指定端口 (默认%d)\n", DEFAULT_PORT);
    printf("  -n <次数>    发送/接收消息次数 (默认%d)\n", DEFAULT_COUNT);
}

// 参数解析
int parse_args(int argc, char **argv, struct write_config *cfg) {
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
    struct rdma_event_channel *ec;      // 事件通道
    struct rdma_cm_id         *cm_id;   // cm id
    struct ibv_pd             *pd;      // 保护域
    struct ibv_comp_channel   *comp_ch; // 完成通道
    struct ibv_cq             *cq;      // 完成队列
    struct ibv_qp             *qp;      // 传输队列对
    struct ibv_mr             *mr;      // 内存注册
    char                      *buf;     // 消息缓冲区
};

// 用于交换 rkey/vaddr 的结构体
struct write_mr_info {
    uint32_t rkey;
    uint64_t vaddr;
};

// 初始化会话资源
int rdma_connection_init(struct rdma_connection *conn, struct write_config *cfg) {
    struct sockaddr_in addr;
    int                ret = 0;

    memset(conn, 0, sizeof(*conn));
    conn->ec = rdma_create_event_channel();
    if (!conn->ec) {
        fprintf(stderr, "rdma_create_event_channel 失败\n");
        return -1;
    }

  /*RDMA_PS_TCP：用于基于 TCP 的 RDMA 通信（如 RoCE、iWARP）。
    RDMA_PS_IPOIB：用于 IP over InfiniBand。
    RDMA_PS_UDP：用于基于 UDP 的 RDMA 通信。
    RDMA_PS_IB：用于原生 InfiniBand 通信。
    */
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
      /*监听
        rdma_listen监听队列的最大长度为 1，当队列满时，新连接请求会被拒绝。
        */
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

// 资源释放
void rdma_connection_cleanup(struct rdma_connection *conn) {
    if (conn->mr)      ibv_dereg_mr(conn->mr);
    if (conn->cq)      ibv_destroy_cq(conn->cq);
    if (conn->comp_ch) ibv_destroy_comp_channel(conn->comp_ch);
    if (conn->pd)      ibv_dealloc_pd(conn->pd);
    if (conn->cm_id)   rdma_destroy_id(conn->cm_id);
    if (conn->ec)      rdma_destroy_event_channel(conn->ec);
    if (conn->buf)     free(conn->buf);
}

// 事件等待
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

// 创建QP等资源
int build_qp(struct rdma_connection *conn) {
    struct ibv_qp_init_attr qp_attr;

    conn->pd = ibv_alloc_pd(conn->cm_id->verbs);
    if (!conn->pd) {
        fprintf(stderr, "ibv_alloc_pd 失败\n");
        return -1;
    }
    //当 CQ 上有完成事件（如发送/接收完成），并且你为 CQ 关联了 comp_channel，内核会通过该 channel 发送事件通知
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

    //创建QP
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq          = conn->cq;
    qp_attr.recv_cq          = conn->cq;
    /*
    IBV_QPT_RC：支持可靠、面向连接的通信，保证数据可靠到达，支持 RDMA 读写和发送/接收。
    IBV_QPT_UC：面向连接但不保证可靠性，支持 RDMA 写和发送/接收，不支持 RDMA 读。
    IBV_QPT_UD：无连接、无可靠性保证，支持一对多通信，常用于广播/多播或管理报文。
    IBV_QPT_RAW_PACKET：允许直接发送/接收原始以太网帧。
    IBV_QPT_XRC_SEND：用于 XRC 发送队列。
    IBV_QPT_XRC_RECV：用于 XRC 接收队列。
    IBV_QPT_DRIVER：用于驱动程序特定的队列类型。
    */
    qp_attr.qp_type          = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = 10;
    qp_attr.cap.max_recv_wr  = 10;
    qp_attr.cap.max_send_sge = 1;//一次发送操作最多能用多少个sge数
    qp_attr.cap.max_recv_sge = 1;//一次接收操作最多能用多少个sge数
    int ret = rdma_create_qp(conn->cm_id, conn->pd, &qp_attr);
    if (ret) {
        fprintf(stderr, "rdma_create_qp 失败\n");
        return -1;
    }
    conn->qp = conn->cm_id->qp;
    return 0;
}

// 注册内存
int reg_mem(struct rdma_connection *conn) {
    if (posix_memalign((void**)&conn->buf, 4096, MSG_SIZE) != 0) {
        fprintf(stderr, "posix_memalign 失败\n");
        return -1;
    }
    memset(conn->buf, 0, MSG_SIZE);
    /*
    IBV_ACCESS_LOCAL_WRITE：允许本地进程写该内存。
    IBV_ACCESS_REMOTE_WRITE：允许远程节点通过 RDMA Write 操作写本地内存。
    IBV_ACCESS_REMOTE_READ：允许远程节点通过 RDMA Read 操作读本地内存。
    IBV_ACCESS_REMOTE_ATOMIC：允许远程节点对本地内存执行原子操作。
    IBV_ACCESS_MW_BIND：允许该内存区域被绑定到内存窗口。
    IBV_ACCESS_ZERO_BASED：允许注册零基址内存。
    IBV_ACCESS_ON_DEMAND：支持按需分页。
    IBV_ACCESS_HUGETLB：允许注册 HugeTLB（大页）内存。
    IBV_ACCESS_RELAXED_ORDERING：允许放宽内存访问顺序。
    */
    conn->mr = ibv_reg_mr(conn->pd, conn->buf, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!conn->mr) {
        fprintf(stderr, "ibv_reg_mr 失败\n");
        return -1;
    }
    return 0;
}

// 服务端主流程
int run_server(struct write_config *cfg) {
    struct rdma_connection   server_conn;
    struct rdma_cm_event         *evt = NULL;
    struct rdma_cm_id            *child = NULL;
    struct rdma_conn_param        conn_param;
    int                           received_msg_count = 0;
    int                           listen_sock = -1, conn_sock = -1;
    struct write_mr_info          local_info, remote_info;
    int                           sock_opt = 1;
    struct sockaddr_in            sin;

    printf("[服务端] 启动，监听 %s:%d，等待连接...\n", cfg->ip, cfg->port);
    if (rdma_connection_init(&server_conn, cfg)){
        fprintf(stderr, "初始化会话资源失败\n");
        return -1;
    }

    /*等待连接请求
    RDMA_CM_EVENT_ADDR_RESOLVED,    地址解析成功。rdma_resolve_addr() 完成，目标地址已转换为 RDMA 设备地址。
    RDMA_CM_EVENT_ADDR_ERROR,       地址解析失败。rdma_resolve_addr() 失败。
    RDMA_CM_EVENT_ROUTE_RESOLVED,   路由解析成功。rdma_resolve_route() 完成，已找到到目标的路由。
    RDMA_CM_EVENT_ROUTE_ERROR,      路由解析失败。rdma_resolve_route() 失败。
    RDMA_CM_EVENT_CONNECT_REQUEST,  收到连接请求。服务端监听时收到客户端的连接请求。
    RDMA_CM_EVENT_CONNECT_RESPONSE, 收到连接响应。客户端发起连接后收到服务端的响应（较少用）。
    RDMA_CM_EVENT_CONNECT_ERROR,    连接建立失败。rdma_connect() 或 rdma_accept() 过程中出错。
    RDMA_CM_EVENT_UNREACHABLE,      目标不可达。通常是网络或路由不可达。
    RDMA_CM_EVENT_REJECTED,         连接被对端拒绝。
    RDMA_CM_EVENT_ESTABLISHED,      连接建立成功。双方可以开始 RDMA 通信。
    RDMA_CM_EVENT_DISCONNECTED,     连接断开。对端主动断开或异常断开。
    RDMA_CM_EVENT_DEVICE_REMOVAL,   RDMA 设备被移除。
    RDMA_CM_EVENT_MULTICAST_JOIN,   成功加入多播组。
    RDMA_CM_EVENT_MULTICAST_ERROR,  加入多播组失败。
    RDMA_CM_EVENT_ADDR_CHANGE,      地址发生变化（如网络热插拔等）。
    RDMA_CM_EVENT_TIMEWAIT_EXIT     连接的 TIMEWAIT 状态结束。*/
    if (wait_event(&server_conn, RDMA_CM_EVENT_CONNECT_REQUEST, &evt)){
        fprintf(stderr, "等待连接请求失败\n");
        goto cleanup;
    }
    child = evt->id;
    //获取到事件后，必须调用 rdma_ack_cm_event() 来“归还”事件，通知内核你已经处理完毕，可以释放相关资源。
    rdma_ack_cm_event(evt);

    // 用子id创建资源
    server_conn.cm_id = child;
    if (build_qp(&server_conn)) {
        fprintf(stderr, "传输队列创建失败\n");
        goto cleanup;
    }

    if (reg_mem(&server_conn)) {
        fprintf(stderr, "rdma 内存注册失败\n");
        goto cleanup;
    }

    // 接受连接
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    if (rdma_accept(server_conn.cm_id, &conn_param)) {
        fprintf(stderr, "rdma_accept 失败\n");
        goto cleanup;
    }

    if (wait_event(&server_conn, RDMA_CM_EVENT_ESTABLISHED, &evt)) {
        fprintf(stderr, "等待连接建立成功事件失败\n");
        goto cleanup;
    }
    rdma_ack_cm_event(evt);

    // 连接建立前，建立 socket 用于 rkey/vaddr 交换
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        fprintf(stderr, "socket 创建失败\n");
        goto cleanup;
    }

    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &sock_opt, sizeof(sock_opt));
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(cfg->port + 1); // 用于参数交换的端口
    if (bind(listen_sock, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        fprintf(stderr, "bind 失败\n");
        goto cleanup;
    }
    listen(listen_sock, 1);

    // 等待客户端 socket 连接
    conn_sock = accept(listen_sock, NULL, NULL);
    if (conn_sock < 0) {
        fprintf(stderr, "accept 失败\n");
        goto cleanup;
    }

    /*vaddr 是指远程主机上注册内存区域（Memory Region, MR）的虚拟地址
     *rkey 是远程主机上注册内存区域的“远程访问密钥”，是 RDMA 设备分配的一个 32 位整数。
     */
    local_info.rkey = server_conn.mr->rkey;
    local_info.vaddr = (uintptr_t)server_conn.buf;
    // 发送本地 rkey/vaddr，接收对方 rkey/vaddr
    if (write(conn_sock, &local_info, sizeof(local_info)) != sizeof(local_info)) {
        fprintf(stderr, "write local_info 失败\n");
        goto cleanup;
    }
    if (read(conn_sock, &remote_info, sizeof(remote_info)) != sizeof(remote_info)) {
        fprintf(stderr, "read remote_info 失败\n");
        goto cleanup;
    }
    close(conn_sock); 
    close(listen_sock);

    printf("[服务端] 连接建立，等待客户端写入...\n");
    // 轮询本地内存，检测数据变化
    char last_buf[MSG_SIZE] = {0};
    while (received_msg_count < cfg->count) {
        if (memcmp(last_buf, server_conn.buf, MSG_SIZE) != 0) {
            printf("[服务端] 收到第 %d 条消息: %s\n", received_msg_count+1, server_conn.buf);
            memcpy(last_buf, server_conn.buf, MSG_SIZE);
            received_msg_count++;
        } 
    }
    printf("[服务端] 消息接收完毕，退出。\n");
cleanup:
    if (conn_sock >= 0) close(conn_sock);
    if (listen_sock >= 0) close(listen_sock);
    rdma_connection_cleanup(&server_conn);
    return 0;
}

// 客户端主流程
int run_client(struct write_config *cfg) {
    struct rdma_connection client_conn;
    struct rdma_cm_event       *evt = NULL;
    struct rdma_conn_param      conn_param;
    struct write_mr_info        local_info, remote_info;
    struct ibv_sge              sge;
    struct ibv_send_wr          wr, *bad_wr = NULL;
    int                         sockfd = -1;
    struct sockaddr_in          sin;
    struct ibv_wc               wc;

    printf("[客户端] 启动，连接 %s:%d...\n", cfg->ip, cfg->port);
    if (rdma_connection_init(&client_conn, cfg)){
        fprintf(stderr, "初始化会话资源失败\n");
        return -1;
    }

    // 地址解析完成
    if (wait_event(&client_conn, RDMA_CM_EVENT_ADDR_RESOLVED, &evt)){
        fprintf(stderr, "地址解析失败\n");
        goto cleanup;
    }
    rdma_ack_cm_event(evt);

    // 路由解析，地址解析成功后调用，成功后才能继续创建 QP 和建立连接。
    if (rdma_resolve_route(client_conn.cm_id, 2000)) {
        fprintf(stderr, "路由解析失败\n");
        goto cleanup;
    }

    //已找到到目标的路由
    if (wait_event(&client_conn, RDMA_CM_EVENT_ROUTE_RESOLVED, &evt)){
        fprintf(stderr, "路由解析事件失败\n");
        goto cleanup;
    }
    rdma_ack_cm_event(evt);

    if (build_qp(&client_conn)){
        fprintf(stderr, "传输队列创建失败\n");
        goto cleanup;
    }
    if (reg_mem(&client_conn)){
        fprintf(stderr, "rdma 内存注册失败\n");
        goto cleanup;
    }

    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    if (rdma_connect(client_conn.cm_id, &conn_param)){
        fprintf(stderr, "rdma_connect 失败\n");
        goto cleanup;
    }

    if (wait_event(&client_conn, RDMA_CM_EVENT_ESTABLISHED, &evt)){
        fprintf(stderr, "等待连接建立成功事件失败\n");
        goto cleanup;
    }
    rdma_ack_cm_event(evt);

    // 等待1s服务器建立
    sleep(1);
    // 连接建立前，连接 socket 用于 rkey/vaddr 交换
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "socket 创建失败\n");
        goto cleanup;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(cfg->ip);
    sin.sin_port = htons(cfg->port + 1); // 用于参数交换的端口
    printf("socket connect 开始\n");
    if (connect(sockfd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        fprintf(stderr, "connect 失败\n");
        goto cleanup;
    }

    // 先接收服务端 rkey/vaddr，再发送本地 rkey/vaddr
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
    close(sockfd);
    sockfd = -1;

    // 组装消息结构
    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uintptr_t)client_conn.buf;
    sge.length = MSG_SIZE;
    sge.lkey   = client_conn.mr->lkey;
    memset(&wr, 0, sizeof(wr));
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_info.vaddr;
    wr.wr.rdma.rkey        = remote_info.rkey;
    printf("[客户端] 连接建立，开始写入消息...\n");
    for (int i = 0; i < cfg->count; ++i) {
        snprintf(client_conn.buf, MSG_SIZE, "%s%d", MSG_STR, i + 1);
        if (ibv_post_send(client_conn.qp, &wr, &bad_wr)) {
            fprintf(stderr, "ibv_post_send (RDMA_WRITE) 失败\n");
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
                // //等待为了均衡服务器处理速度
                // usleep(20000);
                continue;
            }
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[客户端] 完成队列错误: %s\n", ibv_wc_status_str(wc.status));
                goto cleanup;
            }
            if (wc.opcode == IBV_WC_RDMA_WRITE) break;
        }
        printf("[客户端] 已写入第 %d 条消息\n", i+1);
    }
    printf("[客户端] 消息写入完毕，退出。\n");
cleanup:
    if (sockfd >= 0) close(sockfd);
    rdma_connection_cleanup(&client_conn);
    return 0;
}

// 主函数
int main(int argc, char **argv) {
    struct write_config cfg;

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
