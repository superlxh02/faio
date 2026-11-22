# 网络 I/O

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__network.html)

`faio::net` 负责地址的构造与解析、TCP 的监听与全双工读写、UDP 数据报收发，以及把原生句柄的关闭责任交还给调用方。地址与域名解析在 `<faio/net/address.hpp>`，TCP 在 `<faio/net/tcp.hpp>`，UDP 在 `<faio/net/udp.hpp>`，Unix 域套接字和本地管道在 `<faio/net/local.hpp>`。除少数同步返回结果的函数外，网络操作都返回 `task<expected<T>>`，`co_await` 一次即取得值或错误。

## 地址与端点

`Ipv4Addr` 与 `Ipv6Addr` 是定长的地址值，内部按网络字节序保存，复制和比较不涉及堆分配，可以直接作为结构体字段或放在容器里，比较用 `operator==`。两者都提供 `parse()` 从点分或冒号分隔的文本构造，失败返回 `EINVAL`；`to_string()` 输出规范形式，`std::formatter` 已有特化，所以 `std::format("{}", addr)` 可用。`is_unspecified()`、`is_loopback()`、`is_multicast()` 用于在绑定之前做基本判断，IPv4 还可以用 `octets()` 取出四个字节。

`SocketAddr` 携带完整的 IPv4 或 IPv6 端点，内部保存 `sockaddr_storage` 和实际长度，因此 IPv6 的 scope id 与 flow info 不会在转换中丢失。它可以从地址加端口构造、从原生 `sockaddr` 与长度构造，也能反向借出 `sockaddr()` 指针和 `length()` 交给系统调用。`ip()` 返回 `std::variant<Ipv4Addr, Ipv6Addr>`，`port()` 与 `scope_id()` 以主机序返回，`family()`、`is_ipv4()`、`is_ipv6()` 用于分支，`capacity()` 给出 `sockaddr_storage` 的容量，供接收地址的回填缓冲区使用。

```cpp
const auto ip = faio::net::Ipv4Addr::parse("192.0.2.10");
if (!ip)
  co_return;
const faio::net::SocketAddr endpoint{*ip, 8080};
const std::string text = std::format("{}", endpoint); // 192.0.2.10:8080
```

`SocketAddr::parse(host, port)` 与单参数的 `parse(endpoint)` 只做数字解析，不查 DNS，域名应交给 `lookup_host()`。解析 IPv6 时可以带 `%数字` 或 `%接口名` 的作用域，接口名通过 `if_nametoindex()` 转换为索引。单字符串形式要求 IPv6 写成 `[addr]:port`，非方括号形式一旦出现第二个冒号就报 `EINVAL`，端口也必须是 0 到 65535 之间的完整十进制数。

`HostPort` 把主机名与服务绑在一起，两个成员都是 `std::string`，构造时直接复制，所以可以安全地传入临时 `string_view`；服务可以是十进制端口，也可以是 `getaddrinfo()` 认识的服务名。`TcpListener::bind()`、`TcpStream::connect()` 和 `UdpSocket::bind()` 的 `HostPort` 重载会用它发起解析，需要自己拿地址列表时直接调用 `lookup_host()`，它接收 `io_context`、主机名和端口或服务名。当主机与服务都是数字时，解析先走 `SocketAddr::numeric_parse()`，命中后立刻返回单元素结果，不进入线程池；否则在 resolver 执行器上调用 `getaddrinfo()`，worker 线程不参与阻塞查询。结果按 `SocketAddr` 相等去重，空结果报 `EADDRNOTAVAIL`，解析失败的错误来自 `resolver` 错误域。

```cpp
const auto addresses = co_await faio::net::lookup_host(
    faio::io::io_context::current(), "example.com", 443);
if (!addresses)
  co_return;
const auto stream = co_await faio::net::TcpStream::connect((*addresses)[0]);
```

解析结果不做 TTL 缓存，每次调用都可能重新查询并占用 resolver 额度，需要频繁重连的服务应自己保存地址或复用连接。

## TCP

### 建立监听与接受连接

`TcpListener::bind()` 接受数字地址时同步完成，返回 `expected<TcpListener>`；接受 `HostPort` 与 `io_context` 时返回 `task<expected<TcpListener>>`，内部先解析再按顺序尝试每个地址，全部失败才返回错误。`bind()` 也接受 `std::span<const Addr>`，用于同时给出 IPv4 与 IPv6 的候选地址。端口填 0 时，`local_addr()` 给出内核实际分配的端口。

需要先设选项、再绑定本地地址才发起连接或监听时，用 `TcpSocket`：`new_v4()` 与 `new_v6()` 同步创建一个可配置的 socket，`bind()` 绑定本地端点，`listen(backlog)` 把它变成 `TcpListener`，`connect(addr)` 把它变成 `TcpStream`。后两个是右值限定的成员函数，调用后 socket 对象不再可用；已经提交的连接不受父对象析构影响。

`accept()` 默认按 `balanced` 策略为新连接选归属，也可以用 `accept(io_context)` 指定。`accept_options` 有 `placement` 与 `target` 两个字段，语义如下。

| `accept_placement` | 新连接的首次 I/O 归属 |
| --- | --- |
| `listener_local` | 沿用 listener 所在的执行域 |
| `balanced`（默认） | 由运行时放置服务选择同组内的分片；没有放置服务时留在原域，原域已停止则报 `ECANCELED` |
| `explicit_context` | 使用 `options.target`，为空时立即报 `EINVAL`，已停止的目标在注册连接时失败 |

归属的选择和注册发生在 `accept()` 返回流之前，之后把协程或流移到别处都不会改变这条连接的稳定归属，已经接受的连接也不会迁移执行域。`balanced` 在放置服务给不出可用分片时同样报 `ECANCELED`。

`try_accept()` 不挂起，没有待处理连接时返回 `EAGAIN`。`accept_many(maximum, options)` 用于批量领取：它等待第一条连接，随后只取走当前已经排队的那部分，返回 `std::vector<std::pair<Stream, Addr>>`；`maximum` 为 0 报 `EINVAL`。在 `io_uring` 上，批次中后续的接收以 `ACCEPT_DONTWAIT` 提交并等待真实 CQE；能力不支持时它只返回首条，不会为了填满批次一直等待。`epoll` 与 `kqueue` 上则是用非阻塞 `accept()` 排空队列。批次中途遇到真实错误时，已经接受的连接随返回值一起丢弃，数量不会写进 `Error::progress()`；需要保留中间结果就自己循环调用 `try_accept()`。

### 连接与双向读写

`TcpStream::connect()` 有数字地址与 `HostPort` 两种入口，前者直接尝试，后者解析后按输入次序逐个尝试，失败的 socket 在下一次尝试前释放，全部失败才返回最后一次的错误。`connect()` 同样接受地址 span。

读写方向彼此独立，同一个流上可以同时进行一个读操作和一个写操作；同一方向的请求由底层资源控制块仲裁，前一个还没结束时第二个请求会以 `EBUSY` 失败。`read()` 返回本次读到的字节数，空缓冲区上的成功并不表示对端关闭，非空缓冲区读到 0 才表示 EOF；`read_exact()` 读满指定长度，提前遇到 EOF 视为失败。写侧 `write()` 允许短写，需要写完固定内容时用 `write_all()`，零进度写入报 `WriteZero`。`io_buffer` 重载会把缓冲区交给操作，完成时通过 `io_transfer` 带回剩余数据；借用缓冲区的版本不会复制数据，但缓冲区必须活到操作最终完成。

`shutdown(io::ShutdownBehavior::Write)` 只关闭写方向，读方向仍可继续收到对端数据，是回显与半关闭协议常用的做法。`shutdown()` 默认参数就是 `Write`，需要时也可以传 `Read` 或 `Both`。`close()` 返回需要 `co_await` 的等待器：它先把资源标记为关闭，拒绝新的操作，再等在途操作安全结束才释放句柄。关闭完成之前，借用的缓冲区仍要随操作一起存活。

`set_nodelay()`、`set_keepalive()`、`set_linger()`、`set_reuseaddr()` 等选项在流与 listener 上都可用，这类选项的 `getsockopt()` 与 `setsockopt()` 会和资源关闭在同一个执行域临界区内串行，避免句柄被复用后把选项改到别的 socket 上。`take_error()` 读取并清除挂起的 `SO_ERROR`，返回 `expected<std::optional<Error>>`：没有待处理错误时得到 `nullopt`，读取本身失败时得到错误。

### 拆成两个半部

`split()` 借用当前流，返回 `ReadHalf` 与 `WriteHalf`；`into_split()` 消费流，返回 `OwnedReadHalf` 与 `OwnedWriteHalf`。两种半部都持有同一底层资源的共享控制块，父流之后移动或析构也不会留下悬空指针，但借用式半部的文档约定是底层流在整个借用期内存活。

| 半部 | 来源 | 所有权与合并 |
| --- | --- | --- |
| `ReadHalf` / `WriteHalf` | `split()` | 借用；不能合并回 `TcpStream` |
| `OwnedReadHalf` / `OwnedWriteHalf` | `into_split()` | 各自持有资源；可以 `reunite()` |

```cpp
auto [reader, writer] = std::move(stream).into_split();
// 两个半部可以分别 co_await 在各自协程里。
auto merged = std::move(reader).reunite(std::move(writer));
if (!merged)
  co_return;
```

`reunite()` 要求两个半部来自同一次拆分，它比较共享的拆分身份与资源，不匹配时返回 `Error::ReuniteFailed`。合并成功后，写半部的自动关闭被取消，两个半部持有的 socket 与拆分身份被清空，重新得到一个完整的 `TcpStream`。拥有式写半部在析构时，会在在途写操作排空之后请求写方向关闭，相当于隐式 `shutdown(Write)`；如果不希望它关闭写方向，可以在析构前调用 `forget()`，此时资源由其他拥有者继续维持。读半部没有对应的自动行为。

## UDP

`UdpSocket` 可以用 `bind(addr)` 绑定数字地址，或用 `bind(context, HostPort)` 解析后绑定，也可以先用 `unbound(true)` 创建未绑定 socket 设置选项，再连接或绑定。`connect(addr)` 只记录默认目标并设置入包过滤，不发起任何握手；`connect(HostPort)` 解析后逐地址尝试。连接式 socket 可以直接 `send()` 和 `recv()`，未连接的 socket 用 `send_to()` 与 `recv_from()`。

接收接口的差别主要在缓冲区和附带信息，按需选择。

| 调用 | 完成结果 | 说明 |
| --- | --- | --- |
| `recv(span<char>)` | `expected<size_t>` | 借用缓冲区，单包 |
| `recv_from(span<char>)` | `expected<std::pair<size_t, Addr>>` | 借用缓冲区，带来源地址 |
| `recv(io_buffer)` | `task<expected<io_transfer>>` | 拥有型缓冲区 |
| `recv_from(io_buffer)` | `task<expected<OwnedDatagram<Addr>>>` | 拥有型缓冲区，带来源地址与消息信息 |
| `recv_message(buffer, control)` | `expected<DatagramMessage<Addr>>` | 额外报告截断与控制数据长度 |
| `recv_many(span<std::span<char>>)` | `task<expected<std::vector<DatagramMessage<Addr>>>>` | 一次领取已排队的多个数据报 |

`OwnedDatagram` 把接收缓冲区与 `DatagramMessage` 放在一起，缓冲区长度已经按实际收到的字节数设置，可以直接读取。`DatagramMessage` 的 `copied_bytes` 不会超过提交的容量，`truncated` 表示数据报大于缓冲区（或内核报告了 `MSG_TRUNC`），`control_truncated` 与 `control_bytes` 描述控制信息是否被截断。`peek()` 与 `peek_from()` 读取但不消费数据报，`recv_many()` 在收到第一包后立即取走队列里已有的那些，任何失败都会把已消费的数量放进 `Error::progress()`。

发送侧同样提供借用与拥有两种形式。`send_many()` 接收 `std::span<const DatagramSend<Addr>>`，每一项是 `{bytes, peer}`，`peer` 在连接式 socket 上可以留空。整个批次持有写方向，逐条发送并在消息之间让出执行权，返回成功发送的条数，失败时 `Error::progress()` 给出已发送数量，空批次返回 0。`send_message()` 允许自定义 iovec 与控制数据。数据报的边界始终保留，没有把多条消息合并或拆开的接口。

零长度的 UDP 数据报是合法的消息，接收它和 TCP 上读到 EOF 完全不同，不要把 `recv()` 返回 0 当成连接结束。

## 原生句柄与所有权

`owned_native_socket` 是原生 socket 的独占 RAII 所有者，移动独占，`get()` 只借出句柄，`release()` 归还关闭责任，`reset()` 关闭并清空。`validate_import()` 完成平台侧的关联检查，Windows 上用于固定 IOCP 归属。

`into_native()` 要求没有在途操作，成功后所有共享该连接的半部一起脱离注册状态，条件不满足时返回错误。`from_native()` 反向接管一个 `owned_native_socket`，先校验 `SO_TYPE` 与地址族，再注册到给定的 `io_context`。句柄的所有权随实参一起转入：成功时由返回的流或 listener 接管，失败时原生句柄在返回前已经关闭。如果只是临时查询，用 `fd()` 或 `as_native_handle()` 借用，不要在这期间关闭句柄。

Unix 域套接字在 `<faio/net/local.hpp>` 中，`UnixListener` 与 TCP listener 共用同一套 `accept_options`。本地管道用 `faio::net::unix::pipe::Sender` 与 `Receiver` 的 `open()` 打开 FIFO，只接受 `S_ISFIFO` 且访问方向匹配的句柄，普通文件报 `EINVAL`，没有对端时保留内核的 `ENXIO`。

## 超时与错误

网络等待器都提供 `set_timeout(duration)` 与 `set_timeout_at(steady_clock::time_point)`，配置只作用于这次等待，移动等待器不会延长绝对截止时间。超时与普通 I/O 失败都通过 `expected` 返回，已传输的字节数不会回滚，重试前用 `Error::transferred()` 或 `Error::progress()` 判断进度。

错误来源分为 `posix`、`winsock`、`win32` 与 `resolver` 几个域，不能一律按 errno 解释；解析失败、Windows 句柄错误与套接字错误要分别检查 `domain()`。停止请求是协作式的，被停止的等待会以错误结束，但借用的缓冲区仍要等操作真正收尾后才能复用。

开销主要出现在需要汇总结果的接口上：接受、读写与收发本身都是等待器，借用缓冲区的路径不额外建立协程帧；`io_buffer` 重载因为要移交所有权而建立任务，`accept_many()`、`recv_many()` 与 `send_many()` 还需要一次容器分配并把整批条目汇总成单个结果。地址解析只在主机或服务不是纯数字时进入 resolver，并且每次都重新查询。

[TCP 回显](../../examples/net/tcp_echo.cpp) · [UDP 回显](../../examples/net/udp_echo.cpp)
