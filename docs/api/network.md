# 网络 I/O

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__network.html)

类型位于 `faio::net`。按下文的网络操作选择功能头。

## 地址与解析

单独构造或解析地址时包含 `<faio/net/address.hpp>`。TCP 和 UDP 的功能头也提供各自用到的地址类型。

| 接口 | 完成结果 / 用途 |
| --- | --- |
| `address::parse(ip, port)` | `expected<SocketAddr>`，解析数字 IPv4 / IPv6 地址 |
| `lookup_host(host, service, ...)` | 异步地址解析；数字地址可走快路径，其余由独立 resolver 服务处理 |
| `local_addr()` / `peer_addr()` | 获取绑定地址 / 对端地址 |

## TCP

包含 `<faio/net/tcp.hpp>`。

| 接口 | 完成结果 / 行为 |
| --- | --- |
| `TcpListener::bind(address)` | 同步返回监听器结果；需先检查 `expected` |
| `listener.accept()` | 等待连接；成功结果含 stream 与对端地址 |
| `TcpStream::connect(address)` | 异步建立 TCP 连接 |
| `stream.read(span<char>)` | `expected<size_t>`；允许短读，非空缓冲返回 0 表示 EOF |
| `stream.read_exact(buffer)` | 读取至填满缓冲或返回错误；提前 EOF 是失败 |
| `stream.write(span<const char>)` | 单次写入，允许短写 |
| `stream.write_all(buffer)` | 写完全部字节，或返回失败 |
| `TcpSocket::new_v4()` / `new_v6()` | 在 bind / connect 之前设置 socket 选项 |
| `set_nodelay()` 等 socket 配置 | 配置行为受操作系统和资源状态约束 |

### 方向与缓冲区

同一 stream 可以同时执行一个读和一个写；同方向冲突返回 `EBUSY`。借用 buffer 在操作最终完成之前必须存活。需要交付缓冲区所有权时，使用接收 `io_buffer` 的重载；`split()` / `into_split()` 分别得到借用式 / 拥有式的读写半部。

TCP 是字节流。对于按换行分帧的协议，把每次读取追加到接收缓冲，逐条消费完整行，并保留最后一条未完成的行供下一次读取。为帧设置大小上限和读取期限。本机回显使用固定长度消息，调用 `read_exact()` 读满一条已知长度的帧。

## UDP

包含 `<faio/net/udp.hpp>`。

| 接口 | 完成结果 / 行为 |
| --- | --- |
| `UdpSocket::bind(address)` | 创建并绑定 UDP socket |
| `socket.recv_from(buffer)` | 接收一个数据报及来源地址 |
| `socket.send_to(buffer, address)` | 向指定地址发送一个数据报 |
| `socket.connect(address)` | 绑定默认对端，之后可以使用连接态收发 |

UDP 的零长度数据报是有效消息，不应按 TCP EOF 处理。数据报过大、截断、批量收发与组播的支持范围请查询具体声明。

## 平台扩展与错误

Unix 域套接字和本地管道由 `<faio/net/local.hpp>` 按平台能力提供。Windows 的原生句柄与扩展接口遵循 IOCP 生命周期协议。普通网络错误返回 `expected`，错误域可区分 POSIX / Winsock / Win32 / resolver。

可在受支持的可等待读写操作上配置超时，见 [时间与超时](time.md)；组合 task 不一定满足同一 timeout 约束。

完整程序见 [TCP 回显](../../examples/net/tcp_echo.cpp)和 [UDP 回显](../../examples/net/udp_echo.cpp)。
