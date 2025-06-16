### RDMA 关键概念详解

#### 1. 保护域（PD, Protection Domain）
- 保护域是 RDMA 资源隔离的基本单元。所有的内存注册（MR）、队列对（QP）、完成队列（CQ）等都必须属于某个 PD。
- 作用：防止不同应用/进程间的资源误用和越权访问。
- 创建：`ibv_alloc_pd`。

#### 2. 完成队列（CQ, Completion Queue）
- CQ 用于存放 RDMA 操作（如 send/recv、read/write）完成后的事件（Completion）。
- 应用通过 `ibv_poll_cq` 或事件通道获取 CQ 中的完成信息。
- 一个 CQ 可以被多个 QP 共享。
- 创建：`ibv_create_cq`。

#### 3. 队列对（QP, Queue Pair）
- QP 是 RDMA 通信的核心对象，由发送队列（SQ）和接收队列（RQ）组成。
- QP 有多种类型：
  - RC（Reliable Connection，可靠连接）
  - UC（Unreliable Connection，不可靠连接）
  - UD（Unreliable Datagram，不可靠数据报）
- 每个 QP 只能和一个远端 QP 通信（UD 除外）。
- 创建：`ibv_create_qp` 或 `rdma_create_qp`。

#### 4. 工作请求（WR, Work Request）
- WR 是应用向 QP 投递的操作描述符，分为发送 WR（Send WR）、接收 WR（Recv WR）、读写 WR（RDMA Read/Write WR）、原子操作 WR（Atomic WR）等。
- 每个 WR 通过 SGE（Scatter/Gather Entry）描述实际数据缓冲区。
- 发送 WR 通过 `ibv_post_send` 投递，接收 WR 通过 `ibv_post_recv` 投递。

#### 5. 内存注册（MR, Memory Region）
- RDMA 操作前，用户空间的内存必须注册为 MR，获得本地 lkey 和远程 rkey。
- 注册后，HCA 能直接访问该内存。
- 创建：`ibv_reg_mr`。

#### 6. 地址句柄（AH, Address Handle）
- 主要用于 UD 类型 QP，描述远端的寻址信息（如 LID、GID、SL 等）。
- 创建：`ibv_create_ah`。

#### 7. 事件通道（Event Channel）
- RDMA 连接管理和 CQ 通知都可通过事件通道异步通知应用。
- 创建：`rdma_create_event_channel`、`ibv_create_comp_channel`。

#### 8. 连接管理（CM, Connection Manager）
- RDMA_CM 提供了类似 TCP 的连接建立、地址解析、路由解析等机制。
- 相关结构体：`rdma_cm_id`、`rdma_event_channel`。

#### 9. SGE（Scatter/Gather Entry）
- SGE 用于描述一段内存缓冲区（地址、长度、lkey），一个 WR 可以包含多个 SGE，实现分散/聚集 I/O。

#### 10. LID/GID
- LID（Local Identifier）：InfiniBand 网络中的本地标识符。
- GID（Global Identifier）：全局唯一标识，支持 RoCE 等。

#### 11. rkey/lkey
- lkey：本地密钥，描述本地 MR 的访问权限。
- rkey：远程密钥，允许远端主机访问本地 MR。

#### 12. HCA（Host Channel Adapter）
- RDMA 网卡的硬件抽象，负责协议卸载和数据搬运。

enum rdma_cm_event_type {
    RDMA_CM_EVENT_ADDR_RESOLVED, //地址解析成功。rdma_resolve_addr() 完成，目标地址已转换为 RDMA 设备地址。
    RDMA_CM_EVENT_ADDR_ERROR, //地址解析失败。rdma_resolve_addr() 失败。
    RDMA_CM_EVENT_ROUTE_RESOLVED, //路由解析成功。rdma_resolve_route() 完成，已找到到目标的路由。
    RDMA_CM_EVENT_ROUTE_ERROR, //路由解析失败。rdma_resolve_route() 失败。
    RDMA_CM_EVENT_CONNECT_REQUEST, //收到连接请求。服务端监听时收到客户端的连接请求。
    RDMA_CM_EVENT_CONNECT_RESPONSE,//收到连接响应。客户端发起连接后收到服务端的响应（较少用）。
    RDMA_CM_EVENT_CONNECT_ERROR, //连接建立失败。rdma_connect() 或 rdma_accept() 过程中出错。
    RDMA_CM_EVENT_UNREACHABLE, //目标不可达。通常是网络或路由不可达。
    RDMA_CM_EVENT_REJECTED, //连接被对端拒绝。
    RDMA_CM_EVENT_ESTABLISHED, //连接建立成功。双方可以开始 RDMA 通信。
    RDMA_CM_EVENT_DISCONNECTED, //	连接断开。对端主动断开或异常断开。
    RDMA_CM_EVENT_DEVICE_REMOVAL, //RDMA 设备被移除。
    RDMA_CM_EVENT_MULTICAST_JOIN, //成功加入多播组。
    RDMA_CM_EVENT_MULTICAST_ERROR, //加入多播组失败。
    RDMA_CM_EVENT_ADDR_CHANGE,//地址发生变化（如网络热插拔等）。
    RDMA_CM_EVENT_TIMEWAIT_EXIT //连接的 TIMEWAIT 状态结束。
};
-这些事件主要在 rdma_get_cm_event() 获取事件后进行判断和处理。
-通过判断事件类型，应用程序可以决定下一步操作（如建立 QP、注册内存、发送/接收数据、清理资源等）。
-事件机制保证了 RDMA 连接的异步管理和高效资源利用。
struct rdma_conn_param {
    const void *private_data;      // 用户自定义私有数据指针（可用于连接协商时传递额外信息）
    uint8_t private_data_len;      // 私有数据长度（字节，最大 256）
    uint8_t responder_resources;   // 响应方最大并发 RDMA Read/Atomic 操作数
    uint8_t initiator_depth;       // 发起方最大并发 RDMA Read/Atomic 操作数
    uint8_t flow_control;          // 是否启用流控（一般为 0，较少用）
    uint8_t retry_count;           // 连接重试次数（accept 时忽略）
    uint8_t rnr_retry_count;       // RNR（Receiver Not Ready）重试次数
    uint8_t srq;                   // 是否使用共享接收队列（SRQ），若 QP 已创建则忽略
    uint32_t qp_num;               // QP 编号，若 QP 已创建则忽略
};‘
-struct rdma_conn_param 是RDMA 连接管理（RDMA CM）中用于连接协商阶段的参数结构体，主要用于 rdma_connect（客户端发起连接）和 rdma_accept（服务端接受连接）时，双方协商各自能力和连接细节。
-private_data 字段可用于自定义协议扩展或认证。
---