# TCP 回显服务器

本章实现一个 TCP 回显服务器：监听 `127.0.0.1:8080`，接受连接，并持续回显收到的字节，直到对端关闭。完整程序见 [`examples/net/tcp_echo.cpp`](../../examples/net/tcp_echo.cpp)。

## 程序结构

程序由 `serve()`、`handle_connection()` 和 `main()` 三个函数组成。完整代码如下：

```cpp
/**
 * @file tcp_echo.cpp
 * @brief 一个持续监听的 TCP 回显服务端。
 * @details 服务绑定 127.0.0.1:8080，接受连接后交给独立任务处理；每条连接持续
 * 读取字节并原样写回，直到对端关闭或发生错误。按 Ctrl+C 结束进程。
 */
#include <faio/faio.hpp>

#include <array>
#include <exception>
#include <span>
#include <utility>

namespace {
const auto address = faio::net::socket_addr{faio::net::ipv4_addr{127, 0, 0, 1}, 8080};

/** @brief 持续读取一条连接并回写，直到对端关闭或 I/O 失败。 */
faio::task<void> handle_connection(faio::net::tcp_stream stream,
                                   faio::net::socket_addr peer) {
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto count = co_await stream.read(buffer);
    if (!count) {
      faio::log::logger()->error("读取 {} 失败：{}", peer.to_string(), count.error().message());
      co_return;
    }
    if (*count == 0) {
      faio::log::logger()->info("{} 已关闭连接", peer.to_string());
      co_return;
    }

    const auto written =
        co_await stream.write_all(std::span<const char>{buffer}.first(*count));
    if (!written) {
      faio::log::logger()->error("写回 {} 失败：{}", peer.to_string(), written.error().message());
      co_return;
    }
  }
}

/** @brief 绑定端口并持续接受连接。 */
faio::task<int> serve() {
  auto listener = faio::net::tcp_listener::bind(address);
  if (!listener) {
    faio::log::logger()->error("绑定 {} 失败：{}", address.to_string(), listener.error().message());
    co_return 1;
  }

  faio::log::logger()->info("TCP 回显服务监听 {}", address.to_string());
  for (;;) {
    auto accepted = co_await listener->accept();
    if (!accepted) {
      faio::log::logger()->error("接受连接失败：{}", accepted.error().message());
      co_return 1;
    }

    auto [stream, peer] = std::move(*accepted);
    faio::log::logger()->info("接受来自 {} 的连接", peer.to_string());
    faio::spawn_detached(handle_connection(std::move(stream), peer));
  }
}
}  // namespace

/** @brief 运行 TCP 回显服务，退出时返回非零表示启动或接受连接失败。 */
int main() {
  try {
    return faio::block_on(serve());
  } catch (const std::exception& error) {
    faio::log::logger()->error("TCP 回显服务异常退出：{}", error.what());
    return 1;
  }
}
```

## `tcp_listener::bind()` 同步返回

服务入口先绑定地址：

```cpp
auto listener = faio::net::tcp_listener::bind(address);
if (!listener) {
  faio::log::logger()->error("绑定失败：{}", listener.error().message());
  co_return 1;
}
```

`bind(socket_addr)` 不返回任务，而是同步完成绑定并返回 `expected<tcp_listener>`。失败时错误放在 `expected` 里，直接用 `if (!listener)` 处理。`serve()` 返回 `task<int>`，因此绑定失败用 `co_return 1` 结束任务。

## `accept()` 是异步任务

绑定成功后，服务在循环里接受连接：

```cpp
for (;;) {
  auto accepted = co_await listener->accept();
  if (!accepted) {
    faio::log::logger()->error("接受连接失败：{}", accepted.error().message());
    co_return 1;
  }
  auto [stream, peer] = std::move(*accepted);
  faio::spawn_detached(handle_connection(std::move(stream), peer));
}
```

`accept()` 返回可等待任务；`co_await` 后得到 `expected<std::pair<tcp_stream, socket_addr>>`。错误在 `co_await` 的返回值上处理，不会走异常路径。连接流和客户端地址都从 `expected` 中取出。

## `spawn_detached()` 派生连接任务

每个连接交给独立任务处理：

```cpp
faio::spawn_detached(handle_connection(std::move(stream), peer));
```

`spawn_detached()` 提交任务后立即返回，不提供句柄。`tcp_stream` 是移动独占的，必须用 `std::move` 传进处理函数。`accept()` 循环因此只负责接受连接，不会被某个慢客户端拖住。

## `read()` 报告本次读取量

连接处理函数在循环里读取字节：

```cpp
const auto count = co_await stream.read(buffer);
if (!count) {
  faio::log::logger()->error("读取失败：{}", count.error().message());
  co_return;
}
if (*count == 0) {
  co_return;  // 对端关闭
}
```

`read()` 返回 `expected<size_t>`。成功时，值是本次读到的字节数，可能小于缓冲区大小，因此不能假设一次读满。缓冲区非空而返回 0 表示对端关闭；读失败则通过 `expected` 返回错误。正常关闭和 I/O 错误在这里分开处理。

## `write_all()` 写完整段数据

读到数据后，把这一段原样写回：

```cpp
const auto written =
    co_await stream.write_all(std::span<const char>{buffer}.first(*count));
if (!written) {
  faio::log::logger()->error("写回失败：{}", written.error().message());
  co_return;
}
```

`write_all()` 会持续写入，直到这一段字节全部发出。它已经处理短写，因此不需要自己循环调用 `write()`。`std::span` 把本次读到的前缀交给写操作。

## `block_on()` 驱动服务

`serve()` 是 `task<int>`，`main()` 不能写 `co_await`，因此用 `block_on()` 驱动：

```cpp
int main() {
  try {
    return faio::block_on(serve());
  } catch (const std::exception& error) {
    faio::log::logger()->error("TCP 回显服务异常退出：{}", error.what());
    return 1;
  }
}
```

`block_on()` 在当前线程运行服务任务，直到任务结束。任务体抛出异常时，异常会在这里按原类型重新抛出，`try/catch` 负责记录并返回非零退出码。

## 运行

按快速开始中的预设构建 `tcp_echo`。以 macOS 为例：

```sh
cmake --build --preset mac-main-release --target tcp_echo
./target/cmake-build-mac-main-release/examples/tcp_echo
```

另开一个终端发送一行文本：

```sh
printf 'hello faio' | nc 127.0.0.1 8080
```

服务端会回显 `hello faio`，并记录连接的建立和关闭。按 Ctrl+C 结束服务。

## 小结

服务端的处理流程是：`bind()` 建立监听，`accept()` 接受连接，`spawn_detached()` 为每条连接派生任务，`read()` 和 `write_all()` 完成字节读写，`block_on()` 启动服务。所有 I/O 错误都在 `co_await` 的返回值上处理。

[上一章：Hello faio](hello-faio.md) · [返回教程首页](../learn-faio.md)
