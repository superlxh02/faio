# 网络 I/O

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__network.html)

`faio::net` 负责地址构造与解析、TCP 监听与全双工读写、UDP 数据报收发，以及原生句柄所有权。地址在 `<faio/net/address.hpp>`，TCP 在 `<faio/net/tcp.hpp>`，UDP 在 `<faio/net/udp.hpp>`，Unix 域套接字与本地管道在 `<faio/net/local.hpp>`。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 地址与端点 | `ipv4_addr` / `ipv6_addr` | `parse()` 从文本构造，`to_string()` 输出规范形式；含 loopback、multicast 等判断 |
| | `socket_addr` | 完整 IPv4/IPv6 端点；`ip()`/`port()`/`scope_id()`/`family()`/`sockaddr()`/`length()` |
| | `socket_addr::parse` | 只做数字解析；IPv6 用 `[addr]:port`，端口必须在 0 到 65535 |
| | `socket_addr::from_string` | 返回 `expected<socket_addr>`；字符串字面量在编译期校验格式，动态字符串在运行时解析 |
| | `host_port` | 主机名与服务名；构造时复制，临时 `string_view` 也能传入 |
| | `lookup_host(context, host, port)` | 纯数字直接返回，否则进入 resolver；结果去重，空结果返回 `EADDRNOTAVAIL` |
| TCP | `tcp_listener::bind` | 数字地址同步返回 `expected`，`host_port` 重载返回 `task`；端口 0 由 `local_addr()` 给出实际端口 |
| | `accept()` / `accept(context)` | 接受连接；默认按 balanced 策略选择归属，也可指定 context |
| | `accept_options` | `placement` 为 `listener_local`、`balanced` 或 `explicit_context`，后者使用 `target` |
| | `try_accept()` / `accept_many(maximum, options)` | 非阻塞接受 / 批量领取已排队连接；`maximum` 为 0 返回 `EINVAL` |
| | `tcp_stream::connect` | 数字地址或 `host_port`；逐地址尝试，全部失败才返回最后错误 |
| | `read` / `read_exact` | 单次短读 / 读满指定长度；非空缓冲区读到 0 表示 EOF |
| | `write` / `write_all` | 允许短写 / 写完固定内容；零进展写入返回 `WRITE_ZERO` |
| | `io_buffer` 重载 | 缓冲区所有权交给操作，完成时由 `io_transfer` 带回数据 |
| | `shutdown(behavior)` / `close()` | 半关闭指定方向 / 等待在途操作结束后释放句柄 |
| | `split()` / `into_split()` | 借用式半部 / 拥有式半部，拥有式可 `reunite()` |
| | `reunite()` | 合并同一次拆分的两个拥有式半部，不匹配返回 `REUNITE_FAILED` |
| | `set_nodelay` / `set_keepalive` / `set_linger` / `set_reuseaddr` | 套接字选项；与资源关闭在同一临界区串行 |
| | `take_error()` | 读取并清除挂起的 `SO_ERROR`，返回 `expected<optional<error>>` |
| | `tcp_socket` | `new_v4()`/`new_v6()` 后先设选项，再 `bind()`、`listen()` 或 `connect()` |
| UDP | `udp_socket::bind` / `unbound` / `connect` | 绑定地址 / 创建未绑定 socket / 记录默认目标并设置入包过滤 |
| | `recv` / `recv_from` | 借用或拥有缓冲区接收单包，后者带来源地址 |
| | `recv_many` / `peek` / `peek_from` | 一次领取已排队数据报 / 读取但不消费 |
| | `send` / `send_to` | 向默认目标或显式目标发送单个数据报 |
| | `send_many` / `send_message` | 批量发送 `{bytes, peer}` / 自定义 iovec 与控制数据 |
| | `owned_datagram` / `datagram_message` | 接收缓冲区与 `copied_bytes`、`truncated`、控制信息一起返回 |
| 原生句柄 | `owned_native_socket` | 原生 socket 的 RAII 所有者；`get()` 借用，`release()` 归还关闭责任 |
| | `into_native()` / `from_native()` | 转出原生句柄 / 校验类型与地址族后接管 |
| | `unix_listener` / `unix::pipe` | Unix 域套接字与 FIFO 管道，共用 `accept_options` |

## 从字符串构造端点

`socket_addr::from_string` 接受 IPv4 的 `ip:port` 和 IPv6 的 `[ip]:port`。IPv4 每段使用十进制，拒绝多位前导零。直接传字符串字面量时，非法 IP、缺失端口或越界端口会导致编译错误。端点对象在运行时构造，返回值沿用 `faio::expected<socket_addr>`。

```cpp
auto ipv4 = faio::net::socket_addr::from_string("192.0.2.1:8080");
auto ipv6 = faio::net::socket_addr::from_string("[2001:db8::1]:443");
// auto invalid = faio::net::socket_addr::from_string("192.0.2.1:65536");  // 编译错误

std::string configured_endpoint = "192.0.2.1:8080";
auto configured = faio::net::socket_addr::from_string(configured_endpoint);
if (!configured) {
  // 处理 configured.error()，格式错误返回 EINVAL。
}
```

`std::string`、`std::string_view` 和字符指针使用运行时解析。动态字符数组需显式转换为 `std::string_view`，例如 `socket_addr::from_string(std::string_view{buffer})`。IPv6 作用域可写为 `[fe80::1%3]:8080` 或 `[fe80::1%接口名]:8080`；接口名是否存在在运行时检查。主机名解析使用 `lookup_host`。

实现见 [地址接口](../../include/faio/net/address.hpp) 和 [字面量校验](../../include/faio/net/address.hpp)，用例见 [地址测试](../../tests/test_net.cpp) 和 [编译检查](../../tests/test_socket_addr_literal_compile.cpp)。
