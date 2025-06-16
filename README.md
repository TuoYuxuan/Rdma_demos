# RDMA 概念

## 概述

本文档详细介绍了 RDMA（Remote Direct Memory Access）编程中的关键概念、接口和实践。

---

## 🔧 RDMA 核心概念

### 1. 保护域（Protection Domain, PD）

**定义**：保护域是 RDMA 资源隔离的基本单元。

**特性**：
- 所有的内存注册（MR）、队列对（QP）、完成队列（CQ）等都必须属于某个 PD
- 防止不同应用/进程间的资源误用和越权访问
- **创建函数**：`ibv_alloc_pd()`

### 2. 完成队列（Completion Queue, CQ）

**定义**：用于存放 RDMA 操作完成后的事件。

**特性**：
- 存储 send/recv、read/write 等操作的完成信息
- 通过 `ibv_poll_cq()` 或事件通道获取完成信息
- 一个 CQ 可以被多个 QP 共享
- **创建函数**：`ibv_create_cq()`

### 3. 队列对（Queue Pair, QP）

**定义**：RDMA 通信的核心对象，由发送队列（SQ）和接收队列（RQ）组成。

**QP 类型**：
| 类型 | 全称 | 特性 |
|------|------|------|
| RC | Reliable Connection | 可靠连接，提供端到端可靠传输 |
| UC | Unreliable Connection | 不可靠连接，无重传机制 |
| UD | Unreliable Datagram | 不可靠数据报，支持一对多通信 |

**特性**：
- 每个 QP 只能和一个远端 QP 通信（UD 除外）
- **创建函数**：`ibv_create_qp()` 或 `rdma_create_qp()`

### 4. 工作请求（Work Request, WR）

**定义**：应用向 QP 投递的操作描述符。

**WR 类型**：
- **发送 WR**：Send WR
- **接收 WR**：Recv WR  
- **读写 WR**：RDMA Read/Write WR
- **原子操作 WR**：Atomic WR

**特性**：
- 每个 WR 通过 SGE 描述实际数据缓冲区
- **发送函数**：`ibv_post_send()`
- **接收函数**：`ibv_post_recv()`

### 5. 内存注册（Memory Region, MR）

**定义**：RDMA 操作前，用户空间内存必须注册为 MR。

**特性**：
- 注册后获得本地 lkey 和远程 rkey
- HCA 能直接访问该内存
- **创建函数**：`ibv_reg_mr()`

### 6. 地址句柄（Address Handle, AH）

**定义**：主要用于 UD 类型 QP，描述远端的寻址信息。

**特性**：
- 包含 LID、GID、SL 等寻址信息
- **创建函数**：`ibv_create_ah()`

### 7. 事件通道（Event Channel）

**定义**：提供异步事件通知机制。

**特性**：
- RDMA 连接管理和 CQ 通知都可通过事件通道异步通知应用
- **创建函数**：`rdma_create_event_channel()`、`ibv_create_comp_channel()`

### 8. 连接管理（Connection Manager, CM）

**定义**：提供类似 TCP 的连接建立机制。

**特性**：
- 支持地址解析、路由解析等功能
- **相关结构体**：`rdma_cm_id`、`rdma_event_channel`

### 9. 分散/聚集条目（Scatter/Gather Entry, SGE）

**定义**：描述一段内存缓冲区的结构。

**特性**：
- 包含地址、长度、lkey 信息
- 一个 WR 可以包含多个 SGE，实现分散/聚集 I/O

### 10. 网络标识符

| 标识符 | 全称 | 用途 |
|--------|------|------|
| LID | Local Identifier | InfiniBand 网络中的本地标识符 |
| GID | Global Identifier | 全局唯一标识，支持 RoCE 等 |

### 11. 访问密钥

| 密钥类型 | 用途 |
|----------|------|
| lkey | 本地密钥，描述本地 MR 的访问权限 |
| rkey | 远程密钥，允许远端主机访问本地 MR |

### 12. 主机通道适配器（Host Channel Adapter, HCA）

**定义**：RDMA 网卡的硬件抽象，负责协议卸载和数据搬运。

---

## 📡 RDMA CM 事件类型

### 事件枚举定义

```c
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
```

### 事件处理说明

| 事件类型 | 触发条件 | 应用处理 |
|----------|----------|----------|
| `ADDR_RESOLVED` | `rdma_resolve_addr()` 完成 | 进行路由解析 |
| `ROUTE_RESOLVED` | `rdma_resolve_route()` 完成 | 创建 QP，发起连接 |
| `CONNECT_REQUEST` | 服务端收到连接请求 | 决定接受或拒绝连接 |
| `ESTABLISHED` | 连接建立成功 | 开始 RDMA 通信 |
| `DISCONNECTED` | 连接断开 | 清理资源 |

**重要说明**：
- 事件通过 `rdma_get_cm_event()` 获取
- 通过判断事件类型，应用程序可以决定下一步操作
- 事件机制保证了 RDMA 连接的异步管理和高效资源利用

---

## 🔗 连接参数结构体

### rdma_conn_param 结构体

```c
struct rdma_conn_param {
    const void *private_data;      // 用户自定义私有数据指针（可用于连接协商时传递额外信息）
    uint8_t private_data_len;      // 私有数据长度（字节，最大 256）
    uint8_t responder_resources;   // 响应方最大并发 RDMA Read/Atomic 操作数
    uint8_t initiator_depth;       // 发起方最大并发 RDMA Read/Atomic 操作数
    uint8_t flow_control;          // 是否启用流控（一般为 0）
    uint8_t retry_count;           // 连接重试次数
    uint8_t rnr_retry_count;       // RNR 重试次数
    uint8_t srq;                   // 是否使用共享接收队列（SRQ）
    uint32_t qp_num;               // QP 编号
};
```

### 参数详解

| 字段 | 用途 | 备注 |
|------|------|------|
| `private_data` | 连接协商时传递额外信息 | 可用于自定义协议扩展或认证 |
| `private_data_len` | 私有数据长度 | 最大 256 字节 |
| `responder_resources` | 响应方并发操作限制 | 影响 RDMA Read/Atomic 性能 |
| `initiator_depth` | 发起方并发操作限制 | 影响 RDMA Read/Atomic 性能 |
| `retry_count` | 连接重试次数 | `rdma_accept()` 时忽略 |
| `rnr_retry_count` | RNR 重试次数 | Receiver Not Ready 重试 |

### 使用场景

- **`rdma_connect()`**：客户端发起连接时使用
- **`rdma_accept()`**：服务端接受连接时使用
- **连接协商**：双方协商各自能力和连接细节

---

## 📚 相关资源

- [InfiniBand 规范](https://www.infinibandta.org/)
- [RDMA Programming Guide](https://www.openfabrics.org/)
- [libibverbs 文档](https://linux.die.net/man/3/ibv_open_device)

---

**最后更新**：2025年6月