# 分帧

上一章的回显服务靠"双方约定固定长度"来读取数据。真实协议很少这么简单：客户端发来的消息长短不一，而 TCP 只保证字节顺序，不保留消息边界。这一章我们加一层缓冲，把连续的字节流切成一条条消息。

## 什么是分帧

分帧就是把字节流转换成帧序列的过程。一帧是通信双方约定的一个数据单元。比如按换行分帧，每条消息以 `\n` 结尾；HTTP 的帧则包含请求行、头部和正文。

关键是：TCP 的 `read` 可能在任意位置切断消息。你以为对方发了一整行，实际可能只收到半行，甚至一次收到两行半。分帧层要做的就是把这些碎片重新拼成完整的帧，并把多出来的部分留给下一次。

## 用缓冲流读写

faio 提供 `BufReader` 和 `BufWriter` 两个适配器。它们在底层对象之上加一层缓冲，默认容量 8192 字节。我们用内存流来演示，这样不用起网络服务就能跑：

```cpp
#include <faio/faio.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

template <class T>
T checked(faio::expected<T> result, std::string_view step) {
  if (!result)
    throw std::runtime_error(std::string{step} + "失败：" +
                             std::string{result.error().message()});
  if constexpr (!std::is_void_v<T>)
    return std::move(*result);
}

faio::task<void> demonstrate() {
  constexpr std::string_view message{"hello faio"};
  faio::io::MemoryStream stream;
  {
    faio::io::BufWriter writer{stream, 32};
    checked(co_await faio::io::write_all(writer, message), "写入");
    checked(co_await writer.flush(), "刷新缓冲");
  }

  checked(co_await stream.seek(0), "定位到开头");
  faio::io::BufReader reader{stream, 4};
  std::array<char, message.size()> received{};
  checked(co_await faio::io::read_exact(reader, received), "读回完整消息");
  const std::string_view text{received.data(), received.size()};
  if (text != message)
    throw std::runtime_error("读回的字节与写入内容不一致");

  std::array<char, 1> tail{};
  if (checked(co_await reader.read(tail), "检查流结尾") != 0)
    throw std::runtime_error("完整消息后还有额外字节");
  std::cout << text << '\n';
}

}  // namespace

int main() {
  try {
    faio::block_on(demonstrate());
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "内存流示例失败：" << error.what() << '\n';
    return 1;
  }
}
```

## 拆解一下

写入这边：

```cpp
faio::io::BufWriter writer{stream, 32};
checked(co_await faio::io::write_all(writer, message), "写入");
checked(co_await writer.flush(), "刷新缓冲");
```

`write_all` 把字节写进缓冲区，但写进去不等于发出去。字节可能还躺在缓冲区里。`flush()` 才把待写的字节真正提交到底层。这一点很容易忘——`BufWriter` 析构不会自动 flush，退出前必须显式等待 `flush()` 或 `shutdown()`。

读取这边：

```cpp
faio::io::BufReader reader{stream, 4};
checked(co_await faio::io::read_exact(reader, received), "读回完整消息");
```

`BufReader` 会预读，容量 4 意味着它一次最多从底层读 4 个字节。`read_exact` 反复从缓冲区取数据，缓冲空了就再预读，直到填满 `received`。使用者不必关心底层读了几次。

用了预读适配器之后，不要再绕过它直接读底层对象——那样会打乱还没消费的预读内容。

## 读一行

按换行分帧时，`read_line` 比手工拼装方便：

```cpp
std::string line;
const auto size = co_await faio::io::read_line(reader, line, 1024);
if (!size) {
  // EOF 或错误
}
```

它把一行读进 `string`，最多读 `max_bytes` 个字节。faio 还提供了 `read_until(reader, delimiter, output, max_bytes)` 用于按任意分隔符切分。

## 把分帧加到回显服务上

回到我们的 TCP 服务。把 `TcpStream` 用 `BufReader` / `BufWriter` 包起来，就能按行处理消息：

```cpp
faio::task<void> serve_lines(faio::net::TcpStream stream) {
  faio::io::BufReader reader{stream, 1024};
  faio::io::BufWriter writer{stream, 1024};

  std::string line;
  while (co_await faio::io::read_line(reader, line, 4096)) {
    line.push_back('\n');
    co_await faio::io::write_all(writer, line);
    co_await writer.flush();
  }
}
```

注意两个适配器借用同一个 `stream`，所以 `stream` 必须活到它们退出。按行读的好处是不必事先知道消息长度；代价是要为每行设置大小上限，防止对端发来一个没有换行的超长消息把内存吃光——上面 `4096` 就是这个上限。

## 关于 `io_buffer`

如果希望缓冲区的所有权跟着 I/O 操作走，而不是靠调用方保证存活，可以用 `io_buffer`：

```cpp
auto payload = faio::io::io_buffer::copy(std::string_view{"hello"});
auto sent = co_await stream.write(std::move(payload));
if (!sent) co_return;
```

`io_buffer` 在挂起期间自己持有字节和地址，适合缓冲区需要跨任务传递的场景。短读短写时会返回 `io_transfer`，里面有缓冲区和实际进度，方便接着传剩余部分。

## 运行看看

本章完整代码在 [`examples/io/stream.cpp`](../../examples/io/stream.cpp)：

```sh
cmake --build build
./build/my_app
```

输出：

```text
hello faio
```

## 下一步

到目前为止我们一直在用接口。下一章把盖子掀开：一个 `task` 从创建到完成经历了什么，`co_await` 背后那三个方法分别做什么，取消信号又是怎么传到任务里的。搞懂这些，调试异步代码会轻松很多。见[深入 task](task-in-depth.md)。

---

[上一章：网络 I/O](io.md) · [下一章：深入 task →](task-in-depth.md)
