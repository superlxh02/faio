# 生成任务

上一章我们用 `spawn()` 把一个任务交给运行时，但没展开它。这一章我们把 TCP 回显服务搭起来，看看每个连接怎么用独立的任务处理，以及异常和停止请求怎么穿过 `join_handle`。

## 从接受连接开始

一个 TCP 服务首先得监听端口，然后在循环里接受连接。faio 里的写法和同步代码几乎一样，只是把 `accept()` 换成了 `co_await`：

```cpp
#include <faio/faio.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

constexpr std::string_view message{"hello TCP"};

template <class T>
T checked(faio::expected<T> result, std::string_view operation) {
  if (!result)
    throw std::runtime_error(std::string{operation} + "：" +
                             std::string{result.error().message()});
  return std::move(result).value();
}

faio::task<void> run_server() {
  const faio::net::SocketAddr loopback{faio::net::Ipv4Addr{127, 0, 0, 1}, 0};
  auto listener = checked(faio::net::TcpListener::bind(loopback), "绑定 TCP 监听器");

  for (;;) {
    auto [stream, peer] =
        checked(co_await listener.accept().set_timeout(5s), "接受 TCP 连接");
    // 下一章在这里处理连接
  }
}
```

有几处值得停一下：

- `TcpListener::bind(SocketAddr)` 是**同步**的，返回 `expected<TcpListener>`，不带 `co_await`。只有按主机名绑定的那个重载才返回任务。
- `accept()` 是异步的。`co_await` 之后得到 `expected<std::pair<TcpStream, SocketAddr>>`，里面是连接流和客户端地址。
- `.set_timeout(5s)` 挂在等待对象上，表示这次 accept 最多等 5 秒。它必须在任务提交前设置。

那个 `checked()` 是我们自己写的小工具：检查结果，失败就抛出异常带上操作名。faio 的网络接口用 `expected<T>` 表示失败，不会自动变成异常。

## 把每个连接交给一个任务

现在写入真正的处理逻辑。每个连接读了消息就原样写回：

```cpp
faio::task<void> serve_once(faio::net::TcpListener& listener) {
  auto [stream, peer] = checked(co_await listener.accept().set_timeout(5s),
                                "接受 TCP 连接");
  std::array<char, message.size()> bytes{};
  checked(co_await stream.read_exact(bytes), "读取请求");
  checked(co_await stream.write_all(bytes), "写回响应");
  checked(co_await stream.shutdown(faio::io::ShutdownBehavior::Write),
          "关闭服务端写方向");
}
```

`read_exact()` 会一直读到缓冲区填满；`write_all()` 会一直写到全部字节发出去。这两个接口处理了字节流的短读和短写，我们不用自己核对每次实际传输了多少字节。

如果服务只能一次处理一个连接，上面的循环就够用了。但真实服务要同时接待很多客户端。做法是把 `serve_once()` 用 `spawn()` 提交出去：

```cpp
faio::task<void> run_server() {
  const faio::net::SocketAddr loopback{faio::net::Ipv4Addr{127, 0, 0, 1}, 0};
  auto listener = checked(faio::net::TcpListener::bind(loopback), "绑定 TCP 监听器");

  for (;;) {
    auto [stream, peer] =
        checked(co_await listener.accept().set_timeout(5s), "接受 TCP 连接");
    faio::spawn_detached(handle_connection(std::move(stream)));
  }
}
```

`stream` 要 `std::move` 进去，因为 `TcpStream` 是移动独占的。`spawn_detached()` 和 `spawn()` 的区别是：前者不返回结果句柄，适合这种"发射后不管"的连接处理；后者返回 `join_handle<T>`，用于你之后还要领结果的任务。

accept 循环因此不会被某个慢客户端卡住——它只管接连接，读写交给各自的任务。

## 用 join_handle 领结果

有时候你需要知道任务的结果。这时用 `spawn()` 保留句柄：

```cpp
faio::task<int> calculate(int input) {
  co_await faio::time::sleep(10ms);
  co_return input * 2;
}

faio::task<void> show_handle() {
  auto handle = faio::spawn(calculate(21));
  const int value = co_await handle;   // 等待并领取结果
  std::cout << "结果 " << value << '\n';
}
```

`join_handle` 有几个常用的方法：

- `co_await handle` 等待任务结束并取得结果；
- `handle.done()` 查看任务是否已经结束，不阻塞；
- `handle.request_stop()` 提出协作式停止请求；
- `handle.get()` 在普通线程里等待并领结果，worker 内部不能用。

## 异常会沿句柄传播

任务体里抛出的异常会存进任务结果，等到等待句柄的地方再按原类型抛出：

```cpp
faio::task<int> risky(int input) {
  if (input < 0) throw std::invalid_argument("负数不可用");
  co_await faio::this_coro::yield();
  co_return input;
}

faio::task<void> catch_it() {
  auto handle = faio::spawn(risky(-1));
  try {
    (void)co_await handle;
  } catch (const std::invalid_argument& error) {
    std::cout << "捕获到 " << error.what() << '\n';
  }
}
```

这意味着异常不会在 `spawn()` 那一刻冒出来，而是在 `co_await handle` 时。写代码时把等待点放进 `try` 块里。

## 停止是协作式的

`request_stop()` 只是提出请求，不会强行终止任务。任务得自己检查停止令牌：

```cpp
faio::task<void> work_until_stopped() {
  const auto token = co_await faio::this_coro::stop_token();
  for (;;) {
    if (token.stop_requested()) break;
    co_await faio::this_coro::yield_if_needed();
  }
}
```

`yield_if_needed()` 会在需要时让出执行权，让别的任务有机会跑。但要注意：它不会自动抛出取消异常，CPU 循环里必须主动检查令牌。另外，销毁 `join_handle` 不会取消任务——想停就得显式调用 `request_stop()`。

本章的完整可运行代码在 [`examples/net/tcp_echo.cpp`](../../examples/net/tcp_echo.cpp)，那里把客户端和服务端放在同一个程序里，用 `join()` 等两边一起结束。

## 运行看看

```sh
cmake --build build
./build/my_app
```

服务会监听一个由系统分配的本地端口。如果你把客户端代码也放进来（就是完整示例的做法），会看到TCP 回显成功的信息。

## 下一步

现在服务能并发处理连接了，但每个连接看到的都是自己的一份数据。下一章我们加一份所有连接共享的配置，看看 `mutex` 和 `condition_variable` 怎么在协程里用——它们等待时会让出 worker，不占线程。这就是[共享状态](shared-state.md)。

---

[上一章：Hello faio](hello-faio.md) · [下一章：共享状态 →](shared-state.md)
