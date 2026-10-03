# 网络 IO

faio 是 C++23 纯头文件库，用户统一包含 `<faio/faio.hpp>`。网络对象持有后端中立的资源控制块与 `io_context` 租约；Linux 的 io_uring、epoll 和 macOS 的 kqueue 使用相同的 TCP、UDP、Unix socket 与 pipe 接口。Windows 保留原生 IOCP 框架，其网络数据路径不在本版本的支持范围内。

## 1. 架构总览

网络层位于通用 I/O 引擎之上，按"**包装对象 — 资源控制块 — 后端协议**"三层组织：

- **包装对象**（`TcpStream`、`UdpSocket` 等）：面向用户的移动独占句柄，只保存资源控制块的 `shared_ptr` 与 `io_context` 租约，不持有 SQE、CQE、epoll_event 或 kevent 等任何后端结构。
- **资源控制块**（`resource_state`）：fd 所有权、不可变 I/O domain 归属、读写方向执行权、就绪代际与关闭状态。所有异步操作通过它路由到所属 domain。
- **后端协议**：io_uring 以原生 proactor 方式提交请求；epoll/kqueue 通过 readiness adapter 先尝试非阻塞 syscall，遇 `EAGAIN` 登记就绪兴趣后重试。两条路径产生相同的完成结果。

方向执行权（direction gate）是网络层的关键并发约束：同一条 stream 允许一个读和一个写同时进行，同方向冲突返回 `EBUSY`。`ready` 就绪观察与实际读写分开，可以有多个观察者。

## 2. 执行与所有权

网络基础操作构造时只保存参数，在 `co_await` 挂起阶段提交。统一引擎管理资源、操作代际、取消、缓冲区租约和完成交付。

io_uring 是原生 proactor：网络收发、连接和接受请求直接提交给内核，驱动器消费完成事件。epoll/kqueue 使用 readiness adapter：先执行非阻塞 syscall，成功即发布结果；遇到 `EAGAIN`/`EWOULDBLOCK` 登记对应方向，收到 readiness 后继续尝试并产生相同的完成结果。readiness 路径的非阻塞 connect 完成还需要检查 `SO_ERROR` 和已建立的对端状态。`ready` 是独立的就绪观察，io_uring 的观察使用 poll 请求，不消费 socket 数据。

Recv/Send 等待者在调用时捕获资源强租约和紧凑 scalar 参数；右值等待者仍按值拥有，命名左值等待者保持借用。readiness 即时完成无需构造消息头、地址、路径或 iovec 容器；EAGAIN 及原生提交前生成完整、全字段初始化的稳定请求。截止时间、取消、关闭、方向租约与 would-block 代际均进入同一状态机。

原生数据路径为：raw awaiter 保存请求 → `io_uring_prep_*` 填写 SQE → 提交请求 → 等待原始 CQE → 回写结果并恢复或直接继续协程。SQE 可以按批次刷新；请求若在 awaiter 的挂起阶段已经取得 CQE，则直接继续，不额外进入调度队列。通用层只持有稳定请求、原生借用内存、取消与截止时间状态，不执行预先的 read/write syscall，不把原生网络操作交给应用线程池，也不先等待 epoll/kqueue readiness。批量提交和 SQ 容量不足的有界排队仍属于同一原生提交管线。

| 入口 | io_uring 的执行方式 |
| --- | --- |
| `read`/`write`/`peek`、聚集读写、UDP 收发 | 原生 RECV/SEND/RECVMSG/SENDMSG SQE 与 CQE |
| 静态 `TcpStream::connect`、`UnixStream::connect` | 支持 SOCKET opcode 时原生创建句柄，再提交 CONNECT；没有 SOCKET opcode 时仅创建句柄使用短同步 syscall |
| `std::move(TcpSocket).connect`、`UdpSocket::connect` | 对已经创建的 socket 提交原生 CONNECT，不重新创建句柄 |
| `accept`、显式 `shutdown`、异步 `close` | 原生 ACCEPT/SHUTDOWN/CLOSE；关闭等待已接受请求解除内核引用 |
| FIFO `Sender::open`/`Receiver::open` | 原生 OPENAT；管道读写使用 READ/WRITE |
| 显式 `ready` 观察 | 原生 POLL_ADD；它不是内建收发的前置步骤 |
| 同步数字地址 `bind`、`listen`、`new_v4`/`new_v6`、UDP `unbound`、Unix `pair`、options、地址查询和 `try_*` | 非阻塞或短同步控制 syscall；返回 `expected` 的同步合同不伪称为 CQE |
| `async_io(Interest, F)` | 用户提供的非阻塞 syscall 与独立就绪观察组合；未知 callable 不自动转换为原生 opcode |
| `lookup_host` 的非数字主机名 | 没有对应内核 DNS opcode，使用独立 resolver 服务执行 `getaddrinfo` |

IOCP 框架预留原生 OVERLAPPED 请求与完成结果协议，当前在接受实际 I/O 前返回 `ERROR_CALL_NOT_IMPLEMENTED`。它没有线程池模拟的 IOCP 实现，也不报告已经可用的 Windows 网络能力。

带有效 context 的资源创建时确定 I/O domain。runtime 外使用空 context 创建的兼容对象，在首次异步操作时一次发布最终归属；并发首次操作跟随同一个绑定赢家。协程转到其他 worker、socket 被 move 或拆分，都保持已有的 I/O 归属，不重复注册 fd。accept 的结果按照 `accept_options` 选择首次归属，默认在同一 runtime 的活跃 domain 中均衡分配。注册事件使用资源身份，操作使用带代际的 token；关闭后的迟到事件不会作用于复用后的同号 fd。

同一 TCP/Unix stream 允许一个读和一个写同时进行。同方向的冲突返回 `EBUSY`，不会覆盖已有 waiter。`read_exact`、`write_all`、`accept_many` 和 UDP 的收发批次在整个组合期间保持方向 reservation，防止短 I/O、协作让步或分批操作之间插入其他任务。`ready` 观察者与实际读写分开，可以有多个观察者；观察结果允许假就绪。

epoll/kqueue 的标量非阻塞收发可以直接交付即时结果；需要等待时，协程桥先保存恢复目标，再登记稳定操作和消费者。io_uring 的收发始终提交原生 SQE，结果从 CQE 经同一挂起握手交付。

`close()` 是异步关闭：拒绝新操作、请求已有操作取消、排空内存访问并关闭原生 handle。原生 proactor 的取消请求不等于内核已经停止访问内存，只有原始请求完成并排空之后才恢复用户。析构不挂起、不抛异常、不在 worker 等待关闭；最后一个包装拥有者向所属 domain 请求清理。runtime 停止后保留下来的对象可以安全析构，继续 I/O 会返回错误。

数字地址绑定、socket 配置、地址查询属于短控制 syscall，返回 `expected`。配置和查询与关闭共用资源控制临界区，避免查询或配置已被复用的 fd。

## 3. 地址与解析

`faio::net::SocketAddr` 保存 IPv4/IPv6 地址、端口和 IPv6 scope id；`address` 是其别名。`Ipv4Addr`/`v4addr`、`Ipv6Addr`/`v6addr` 提供纯数字解析、字符串转换和地址分类。

```cpp
auto v4 = faio::net::address::parse("127.0.0.1", 8080);
auto v6 = faio::net::address::parse("[::1]:8080");
auto scoped = faio::net::address::numeric_parse("fe80::1%en0", 8080);
```

`parse` 不执行 DNS。`lookup_host(context, host, service)` 异步返回全部可用的 IP 端点；service 可以是十进制端口或系统服务名。数字 host 与数字 port 走同步纯解析快路径，域名解析使用独立 resolver executor 配额，worker 不执行阻塞 `getaddrinfo`。输入按值拥有并拒绝内嵌 NUL；解析器错误通过 `Error::domain()==error_domain::resolver` 保留。

```cpp
auto ctx = faio::io::io_context::current();
auto endpoints = co_await faio::net::lookup_host(ctx, "localhost", "8080");
auto stream = co_await faio::net::TcpStream::connect(
    ctx, faio::net::HostPort{"localhost", 8080});
```

`HostPort` 拥有 host/service 字符串。TCP 多地址连接和 hostname 连接按解析顺序尝试；失败连接的资源进入关闭协议。数字地址及地址 span 可直接使用，span 的存储必须覆盖组合操作生命周期。

## 4. TCP

### 4.1 TcpSocket

`TcpSocket` 是连接或监听以前的配置对象，move-only。

| 操作 | 返回值与行为 |
| --- | --- |
| `new_v4(ctx)`、`new_v6(ctx)` | `expected<TcpSocket>`，创建 nonblocking/CLOEXEC socket |
| `bind(address)` | 绑定本地地址，端口 0 由内核分配 |
| `std::move(socket).connect(address)` | 立即消费 socket 到组合协程帧，返回 `task<expected<TcpStream>>`；等待后得到连接结果 |
| `std::move(socket).listen(backlog)` | 返回 `expected<TcpListener>`，失败保留 socket |
| `local_addr`、`peer_addr`、`take_error` | 查询端点或读取并清除 `SO_ERROR` |
| `from_native`、`into_native`、`as_native_handle` | 原生拥有者互操作与非拥有型观察 |

`new_v4`/`new_v6` 是同步创建接口；随后配置、绑定与 `listen` 也保持同步 `expected` 合同。配置对象的异步 `connect` 只连接这个已经拥有的 socket，io_uring 提交 CONNECT。静态 `TcpStream::connect` 则在连接组合中先通过统一 SOCKET awaiter 创建句柄，再执行 CONNECT。

配置包括 `reuseaddr`、`reuseport`、`nodelay`、`keepalive`、`linger`、收发 buffer size、IPv6 only、IP TTL/hop limit 及对应 setter。OS 可能调整实际 buffer size，查询返回内核值。`linger(nullopt)` 关闭 linger，零秒为 abortive close，正秒保持 OS 关闭等待语义。io_uring 关闭提交原生 CLOSE 请求并等待 CQE；epoll/kqueue 可能阻塞的关闭使用 cleanup lane，不占用协程 worker。

### 4.2 TcpListener

`bind(address, ctx, backlog)`/`bind(ctx, address, backlog)` 同步创建、绑定并监听 socket；`bind(ctx, HostPort, backlog)` 返回组合 task，先异步解析，再依次调用同一同步绑定接口。地址 span 绑定按顺序尝试。

`accept()` 返回 `expected<pair<TcpStream, SocketAddr>>`。接受的 socket 会设置 nonblocking/CLOEXEC 及平台的 SIGPIPE 保护，并在返回 stream 前确定唯一、稳定的 I/O domain。默认 `accept_options{}` 使用 `accept_placement::balanced`，在同一 runtime 的活跃 worker domain 中轮转分配；独立引擎使用自身。`listener_local` 明确继承 listener 的实际 domain，`explicit_context` 使用配置的 `target`；`accept(target_context)` 是显式策略的便利重载。空的显式 target 返回 `EINVAL`，停止的目标或全组停止返回 `ECANCELED`，新 fd 由原生 guard 安全清理。

这些选项仅用于新接受 fd 的首次注册，已发布 stream 始终保持该归属，协程迁移不会迁移 fd。一个 accept awaiter 的结果只允许消费一次，重复取结果返回 `EINVAL`——不能为同一个 fd 创建两个关闭责任。`try_accept(options)` 是显式的即时同步尝试。`accept_many(maximum, options)` 等待第一条连接，再领取当前已排队的连接，返回有界批次，每个新连接独立应用相同归属策略。这两个接口也提供显式 context 重载。TCP 和 Unix listener 共用此配置。

io_uring 批次的后续接受使用带 `IORING_ACCEPT_DONTWAIT` 的原生 ACCEPT SQE，直到 CQE 返回 `EAGAIN` 或到达 maximum。该标志自 Linux 6.10 提供，`capabilities().native_accept_nowait` 报告实际后端的缓存能力；缺少此标志的内核返回已经原生完成的首条，不改用同步 accept，也不为填满批次等待新的连接。epoll/kqueue 使用非阻塞 accept syscall 排空已有连接。

监听器提供 `local_addr`、`ready`/`readable`、socket options、`close` 和原生导入导出。

### 4.3 TcpStream 与缓冲区

| 功能 | 接口 |
| --- | --- |
| 读写 | `read`、`write`、`peek`、`read_vectored`、`write_vectored` |
| 即时尝试 | `try_read`、`try_write`、`try_peek`、`try_read_vectored`、`try_write_vectored` |
| 组合 | `read_exact`、`write_all`、`write_all_buf`、`read_owned`、`write_owned` |
| 初始化范围 | `read_buf(vector<char>&, count)`、`read_buf(io::read_buf&)` 及 try 版本 |
| 就绪 | `ready(Interest)`、`readable`、`writable` |
| 自定义操作 | `try_io(Interest, F)`、`async_io(Interest, F)` |
| 生命周期 | `shutdown(direction)`、`flush`、`close`、native import/export |

基础操作允许短读、短写。非空 buffer 的读返回零表示 EOF；空 TCP buffer 在资源与方向验证后成功返回零，不等待对端数据，也不表示 EOF。全空的聚集范围遵循相同规则；UDP 不使用这项流语义。`read_exact` 遇到中途 EOF 返回 `UnexpectedEOF`；`write_all` 遇到零进度返回 `WriteZero`。错误的 `progress()` 保留组合此前已生效的字节数，取消不回滚已传输数据。

`span<char>`、`span<const char>` 是零拷贝借用，调用者必须保证内存有效直到操作排空，且没有并发读写冲突。`borrowed_buffer` 和 `borrowed_const_buffer` 显式表达该约束。

`read(io_buffer)`、`write(io_buffer)` 移入独占拥有缓冲区，完成后返回 `io_transfer{buffer, bytes}`；读操作只发布已初始化范围。`write(shared_const_buffer)` 允许共享不可变 payload。拥有型操作在调用方法时抓取稳定资源引用，socket wrapper 随后被移动不会留下悬空 `this`。

聚集接口可以接收 iovec span 或多个连续字符范围；消息头和内置 iovec 位于最终 awaiter 中，提交前移动会重建指针，不复制 payload：

```cpp
// VectoredRead 的移动构造：重建 operation 使原生消息指针指向新 awaiter
VectoredRead(VectoredRead &&other) noexcept
    : resource_{std::move(other.resource_)}, vectors_{other.vectors_},
      message_{/* msg_iov 指向本对象的 vectors_ */},
      operation_{io::recvmsg(resource_, &message_, 0)} {}
```

调用者提供的外部 payload、iovec 数组和 control buffer 必须满足借用生命周期。

`write_zc` 在支持 SEND_ZC 的 io_uring 上对 TCP 选择原生零拷贝请求，在 epoll/kqueue、Unix stream 或缺少此 opcode 的内核上使用普通安全发送路径。原生请求即使已经报告发送字节数，也要等待内核的 buffer-release 通知之后才返回，调用者此前不能修改或复用借用 buffer；取消同样等待排空。它允许短写，并支持与基础操作相同的 deadline 配置。`io::send_zc`/`io::sendmsg_zc` 是显式原生扩展，不支持的后端返回 `EOPNOTSUPP`。

`flush` 没有额外用户态 TCP 缓存，不承诺对端已经收到、读取或确认数据。

### 4.4 就绪与自定义 syscall

```cpp
for (;;) {
    auto result = stream.try_read(buffer);
    if (result) break;
    if (result.error().value() != EAGAIN &&
        result.error().value() != EWOULDBLOCK) {
        // 处理真实错误。
        break;
    }
    auto observed = co_await stream.readable();
    if (!observed) break;
}
```

等待 `ready` 后得到 `expected<io::Ready>` 就绪快照，不消费数据。`is_readable()`、`is_writable()` 与 `is_error()` 查询相应提示；`is_read_closed()` 和 `is_write_closed()` 分别记录已经观察到的读、写方向关闭，不能据此假定另一个方向也关闭。读 EOF 提示仍可能伴有尚未消费的数据，最终 EOF 由非空 buffer 的真实 read 返回零确认。

kqueue 按 read/write filter 分别解释 `EV_EOF`，本地写半关闭产生的写 EOF 不使 `readable()` 完成；Linux 的 RDHUP 只提示读方向关闭，全 HUP 同时提示两个方向。观察依据所请求方向或错误完成，关闭信息单独保留。try 操作遇到 would_block 会清除该方向的旧就绪位，保留已观察到的关闭信息；重试循环需要等待后续对应方向事件。

`try_io`/`async_io` 的函数返回 `expected<T>`，可以接收 fd 参数或无参数，必须是短的非阻塞 syscall，且不能递归调用同一对象的包装方法。`async_io` 是显式的自定义 syscall 兼容接口，在 would_block 后等待并重试：

```cpp
static auto async_io_impl(std::shared_ptr<io::detail::resource_state> resource,
                          io::Interest interest, F function) -> task<Result> {
  while (true) {
    auto result = io::detail::with_resource(resource, interest, [&] { ... });
    if (result || (result.error().value() != EAGAIN &&
                   result.error().value() != EWOULDBLOCK))
      co_return result;
    auto readiness = co_await io::ready(resource, interest);
    if (!readiness)
      co_return std::unexpected{readiness.error()};
  }
}
```

内建 read/write/accept/UDP 收发不经过它。延迟执行的组合接口在调用时按值保存资源与 callable，不借用包装对象的 `this`；移动包装对象不会影响 task。最后一个包装拥有者析构仍然会请求关闭，尚未启动的 task 随后安全返回关闭或取消错误。

基础读写、accept、UDP 收发及聚集 awaiter 支持 `set_timeout(duration)` 和 `set_timeout_at(steady_clock::time_point)`。到期返回 `ETIMEDOUT`；停止令牌返回 `ECANCELED`。返回以前后端已停止访问借用缓冲区。

### 4.5 split 与半关闭

`split() &` 返回 `ReadHalf`、`WriteHalf`。半边保留控制块，父对象 move 不会造成裸指针悬空；父 stream 与半边仍不能并发使用同一方向。

`std::move(stream).into_split()` 返回 `OwnedReadHalf`、`OwnedWriteHalf`，可以分别交给不同任务。`reunite` 比较资源与 split identity；不匹配返回 `ReuniteFailed`，两半边保留原有拥有权。

owned 写半边析构在此前写入排空后请求写半关闭。`forget()` 取消这项自动请求。读半边析构不会强制关闭整个连接；最后一个包装拥有者触发资源关闭。`shutdown(Write)` 只关闭写方向，读方向仍可以接收；`close` 关闭整个资源。

## 5. UDP

`UdpSocket` 是 move-only 消息型对象，`UdpDatagram` 是兼容别名。数字地址 `bind(address, ctx)`、`bind(ctx, address)` 与 `unbound` 均同步返回 `expected<UdpSocket>`；`unbound` 可以在配置后关联目标。`bind(ctx, HostPort)` 先异步解析，再调用同一同步创建与绑定接口。

异步 `connect(peer)` 设置默认目标和接收来源过滤，保持 UDP 消息边界；io_uring 对已有 socket 提交原生 CONNECT，不执行 TCP 握手。`send`/`recv` 用默认目标，`send_to`/`recv_from` 使用显式地址；`recv_from` 返回 `pair<copied_bytes, peer>`。

UDP 零长度包是合法消息。网络层向零容量接收 buffer 收包仍会消费一条数据报：等待者在自身稳定存储中提供一个丢弃字节，保留消息边界，返回的 copied_bytes 仍限制在用户容量内；peek 保留原报文。低层 `io::recv` 直接保留原生系统调用语义，macOS 的零容量 recv 不消费报文，验证零长度报文是否被消费时应提供非零接收容量。基本接收 API 可能截断，应用需要完整判断时使用 `recv_message`。

| 功能组 | 接口 |
| --- | --- |
| 即时/窥视 | `try_send`、`try_send_to`、`try_recv`、`try_recv_from`、`peek`、`peek_from` 与 try 版本 |
| buffer | span、borrowed、io_buffer、shared_const_buffer；`recv_buf`、`recv_buf_from` 与 try 版本 |
| 就绪/扩展 | `ready`、`readable`、`writable`、`try_io`、`async_io` |
| 消息 | `send_message(iovecs, optional<address>, control, flags)`、`recv_message(buffer, control, flags)` 与 try 版本 |
| 批量 | `send_many(span<const DatagramSend>)`、`recv_many(span<span<char>>)` |
| 共享 | `share()` 创建使用同一受控资源的拥有者 |
| 配置 | broadcast、IPv4/IPv6 multicast membership/loop/TTL/hops/interface、TTL、IPv6 only、buffer size |
| 辅助数据 | `set_recv_packet_info_v4/v6` 配置原生目标地址/接口 control message |

`DatagramMessage` 包含 `copied_bytes`、`peer`、`truncated`、`control_truncated`、`control_bytes` 和原生 `flags`。`MSG_TRUNC` 可能让系统返回完整包长度，包装始终把 copied_bytes 限制在有效 buffer 内。control buffer 保存本平台原生 `cmsghdr`，其对齐与 payload 解析由调用者负责。

`recv_from(io_buffer)` 返回 `OwnedDatagram{buffer, message}`；缓冲区在任务帧中独占拥有。`send_many` 逐消息发送，失败的 `progress()` 表示已发出的消息数量。`recv_many` 只等待第一包，后续做即时尝试，因此允许部分批次；调用者继续提交剩余 buffer 即可。io_uring 的后续项使用 `MSG_DONTWAIT` RECVMSG SQE 与真实 CQE，`EAGAIN` 结束本批；epoll/kqueue 使用显式同步 try 路径排空已有消息。

多个发送任务使用 `share()` 可以共享资源，但同方向重叠操作依然返回 `EBUSY`。多个消费者需要单接收循环与有界 channel 分派；库不替调用者承诺不同接收任务的消费顺序。

## 6. Unix sockets 与 pipe

`faio::net::unix::address`/`UnixAddr` 支持 pathname 和 unnamed。`abstract(name)` 为 Linux 显式扩展，macOS 返回 `ENOTSUP`。pathname 拒绝内嵌 NUL 和超长路径。socket 析构不删除文件系统路径，由创建者明确管理路径清理。

`UnixStream`/`UnixListener` 复用字节流、就绪、try、split、native 与关闭协议。`UnixStream::pair(ctx)` 创建非阻塞全双工 socketpair；`peer_credentials()` 提供 UID/GID，Linux 同时返回 PID，macOS 的 PID 为 `nullopt`。

`UnixDatagram` 提供 `bind`、`unbound`、`pair`、connected/unconnected 收发、peek、消息辅助数据、shutdown、native 和有界 batch。`UnixSocket::stream/datagram` 允许创建后配置/绑定，再转换为 listener、stream 或 datagram。

`faio::net::unix::pipe::pair(ctx)` 返回 `pair<Sender, Receiver>`。发送端提供 `write`/`write_all`/`writable`，接收端提供 `read`/`read_exact`/`readable`，均有 borrowed/owned buffer 及 native import/export。`open(ctx, path)` 提交统一 OPEN 请求：io_uring 使用原生 OPENAT SQE/CQE，epoll/kqueue 使用文件服务打开 nonblocking FIFO；发送端没有接收端时返回内核 `ENXIO`。

pipe 的原生导入会验证 FIFO/匿名管道类型和访问方向，不接受可能阻塞 reactor 的普通文件。pipe 不使用 socket recv/send。写端全部关闭后，接收端排空已有内容再读到 EOF；读取端已关闭时写返回 `EPIPE`。macOS 使用 fd 的 `F_SETNOSIGPIPE`，Linux 使用系统调用线程的局部信号保护；库不修改进程全局 SIGPIPE handler。大于 `PIPE_BUF` 的多写者数据不承诺原子性。

泛型可非阻塞 fd 的注册与 readiness guard 见 `io::unix::AsyncFd` 和[异步IO](异步IO.md)。普通磁盘文件通过 filesystem provider 选择执行方式：io_uring 对具备原生 opcode 的操作直接提交文件 SQE/CQE，epoll/kqueue 使用独立文件服务；普通文件不通过 readiness reactor 伪装异步。

## 7. 运行示例与测试

`examples/tcp_echo_server.cpp`、`examples/udp_echo_server.cpp` 展示基础持续 echo server；`examples/tcp_echo_server_single_thread.cpp` 在单个 cpp 内独立实现完整 TCP echo 逻辑，改由调用 block_on 的当前线程调度。

`examples/tcp_counter_server.cpp` 是多个客户端共享的内存计数器服务，支持 SET、GET、INCR、DEL 和 QUIT。它展示基于换行的分帧、BufReader 处理拆分和流水线请求、协程 mutex 保护原子递增、请求长度限制、完整请求与发送期限，以及客户端错误和断开的处理。默认监听 `127.0.0.1:8081`，可用 nc 交互；`--self-test` 绑定临时端口，运行两个并发计数客户端和一个协议校验客户端后自动退出。

Linux 示例接受 `--io-backend=uring`、`--io-backend=epoll`，如 `tcp_counter_server --self-test --io-backend=epoll`。不传参数保留库的默认选择规则；也可通过 `FAIO_TEST_IO_BACKEND=uring|epoll` 选择，显式参数优先。macOS 固定使用 kqueue。tcp_echo_server/tcp_counter_server 使用四个 worker，udp_echo_server 使用两个 worker，tcp_echo_server_single_thread 不创建后台协程 worker。

详细中文注释与交互步骤见 [示例指南](../examples/README.md)。高级接口的验证按独立契约测试组织，见 `tests/test_network_contract.cpp`。

网络契约测试覆盖 current_thread/multi_thread 两种模式；Unix 契约测试覆盖 pair、pathname、半关闭、重合身份、拥有型操作延迟启动、credentials、FIFO、EPIPE 与 EOF；结构化取消测试覆盖 scope 失败后的 body 取消与条件变量取消后的安全重获 mutex。
