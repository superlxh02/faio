# 网络 I/O

前面几章我们搭了服务的骨架。这一章把 TCP 回显服务真正写完，顺便弄清 `TcpStream` 的读写接口：为什么读有 `read` 和 `read_exact` 两个版本，EOF 怎么判断，超时挂在哪里。

## 完整的回显程序

这是仓库里的 `examples/net/tcp_echo.cpp`。它把客户端和服务端放在同一个程序里，跑一次就能验证整条链路：

```cpp
#include <faio/faio.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace std::chrono_literals;

constexpr std::string_view message{"hello TCP"};

template <class T>
T checked(faio::expected<T> result, std::string_view operation) {
  if (!result)
    throw std::runtime_error(std::string{operation} + "：" +
                             std::string{result.error().message()});
  return std::move(result).value();
}

faio::task<void> serve_once(faio::net::TcpListener& listener) {
  auto [stream, peer] =
      checked(co_await listener.accept().set_timeout(5s), "接受 TCP 连接");
  std::array<char, message.size()> bytes{};
  checked(co_await stream.read_exact(bytes), "读取请求");
  checked(co_await stream.write_all(bytes), "写回响应");
  checked(co_await stream.shutdown(faio::io::ShutdownBehavior::Write),
          "关闭服务端写方向");
}

faio::task<void> request_echo(faio::net::SocketAddr destination) {
  auto stream = checked(co_await faio::net::TcpStream::connect(destination),
                        "连接 TCP 服务");
  checked(co_await stream.write_all(std::span<const char>{message}), "发送请求");

  std::array<char, message.size()> response{};
  checked(co_await stream.read_exact(response), "读取响应");
  if (std::string_view{response.data(), response.size()} != message)
    throw std::runtime_error("TCP 回显内容不匹配");

  std::array<char, 1> tail{};
  if (checked(co_await stream.read(tail).set_timeout(5s), "确认响应结束") != 0)
    throw std::runtime_error("TCP 回显包含多余字节");
}

faio::task<void> run_echo() {
  const faio::net::SocketAddr loopback{faio::net::Ipv4Addr{127, 0, 0, 1}, 0};
  auto listener = checked(faio::net::TcpListener::bind(loopback), "绑定 TCP 监听器");
  const auto destination = checked(listener.local_addr(), "查询 TCP 监听地址");
  co_await faio::join(serve_once(listener), request_echo(destination));
  checked(co_await listener.close(), "关闭 TCP 监听器");
}
}  // namespace

int main() {
  try {
    faio::block_on(run_echo());
    std::cout << "TCP 回显：" << message << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
```

## `read` 和 `read_exact` 的区别

这是最容易搞混的一对接口。

`read(buffer)` 是**一次**读取，能读多少读多少。它返回 `expected<size_t>`，表示这次实际读到的字节数。TCP 是字节流，一次 `read` 不保证填满缓冲区——对方发 9 个字节，你可能会先收到 5 个，剩下的下一趟才到。所以必须检查返回值。

`read_exact(buffer)` 是**读满**为止：短读了就继续读，直到缓冲区填满。提前遇到 EOF 则报错。它返回 `expected<void>`，因为成功时缓冲区一定是满的，不需要额外返回字节数。

写这边同理：`write(buffer)` 是一次写入，可能短写；`write_all(buffer)` 会一直写到全部字节发出去。

回显程序里双方约定了固定长度的消息，所以用 `read_exact` 和 `write_all` 最省事，不必自己处理短读短写：

```cpp
std::array<char, message.size()> bytes{};
checked(co_await stream.read_exact(bytes), "读取请求");
checked(co_await stream.write_all(bytes), "写回响应");
```

## EOF 怎么判断

客户端读完响应后，还要确认服务端不会再发东西了。这里用的是：

```cpp
std::array<char, 1> tail{};
if (checked(co_await stream.read(tail).set_timeout(5s), "确认响应结束") != 0)
  throw std::runtime_error("TCP 回显包含多余字节");
```

关键是"**非空**缓冲区的 `read` 返回 0"。这表示对端已经关闭写方向，而且没有更多字节了，也就是 EOF。如果缓冲区本身就是空的，返回 0 不代表任何含义。

服务端那边显式关掉了写方向：

```cpp
co_await stream.shutdown(faio::io::ShutdownBehavior::Write);
```

这一半关闭会让客户端的 `read` 看到 EOF。注意 `shutdown` 是关闭写方向，不是关闭整个连接——读方向还能继续用。

## 超时挂在哪里

程序里出现了两次 `.set_timeout(5s)`：

```cpp
co_await listener.accept().set_timeout(5s)
co_await stream.read(tail).set_timeout(5s)
```

超时是挂在**等待对象**上的，不是挂在连接上。它只对这一次等待生效，而且必须在操作提交之前设置。写法是链式的，紧跟在 `accept()`、`read()` 后面。

底层有两套 `set_timeout`：网络等待对象接受任意 `chrono` 时长，所以 `5s` 能直接用；更底层的 I/O 请求等待对象只接受 `std::chrono::milliseconds`。日常写网络代码用前者就够了。

另一种写法是用 `faio::time::timeout()` 把超时和 I/O 操作包在一起：

```cpp
auto result = co_await faio::time::timeout(
    stream.read(buffer), std::chrono::milliseconds{1000});
```

两者的语义一样：超时也是一种失败，要通过 `expected` 处理。

## 错误里带着进度

`expected` 失败时，`Error` 除了 `message()`，还有几个值得注意的方法：

```cpp
error.message();     // 可读的错误描述
error.domain();      // 错误域：posix / win32 / winsock / resolver / ...
error.value();       // 域内的原始整数码
error.transferred(); // 失败前已经传输的字节数
```

`transferred()` 很重要：一次 I/O 失败或取消，可能已经有部分字节被读写了，系统调用的副作用不会回滚。重试之前先看这个数字，判断该从哪里接着传，不能假设什么都没发生。

## 读写半边

同一条 stream 可以同时挂一个读和一个写，同方向的两个操作会返回 `EBUSY`。如果你想把读写拆给两个任务分别处理，可以用：

```cpp
auto [reader, writer] = std::move(stream).into_split();
```

`into_split()` 消费原 stream，返回拥有式的读半边和写半边，可以分别 move 给不同协程。 borrowed 版本 `split()` 返回借用式半边，要求父 stream 活过半边。必要时还能用 `reunite()` 把拥有式半边合回去。

## 运行看看

```sh
cmake --build build
./build/my_app
```

输出：

```text
TCP 回显：hello TCP
```

把 `message` 改成别的字符串再跑一次，回显内容会跟着变。

## 下一步

现在这个服务只认固定长度的消息。真实协议通常没有这么奢侈——客户端发什么长度是不定的。下一章我们加一层缓冲，把字节流切成一条条消息，这就是[分帧](framing.md)。

---

[上一章：通道](channels.md) · [下一章：分帧 →](framing.md)
