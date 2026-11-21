# 异步流

这是教程的最后一章。前面我们处理的都是"一次性"的异步操作：等一个结果，或者等一组固定的结果。这一章看另一种模式——数据一项接一项地来，你要一边读一边处理，而且可能限制同时处理几项。

## 一个简单的序列

faio 的异步流由一个生产端和一个消费端组成。生产端用 `emit()` 逐项交付，消费端用 `next()` 逐项读取。两者由 `with_stream()` 组装在一起：

```cpp
#include <faio/faio.hpp>
#include <atomic>
#include <iostream>
#include <optional>
#include <stdexcept>

faio::task<void> produce(faio::stream_producer<int>& output) {
  for (int value = 1; value <= 5; ++value) {
    co_await output.emit(value);
  }
}

faio::task<void> show_stream() {
  co_await faio::with_stream<int>(
      produce,
      [](faio::stream_reader<int>& input) -> faio::task<void> {
        while (const auto value = co_await input.next()) {
          std::cout << "依次读取 " << *value << '\n';
        }
      });
}
```

`emit()` 可以挂起——交付一项之后，它会等到消费端来取下一项才继续。这就是背压：生产快不过消费时自然慢下来，不会把数据无限堆在中间。`next()` 在生产端正常结束时返回 `nullopt`，循环就此结束；生产端抛出的异常会原样传播到消费端。

## 提前退出会收尾生产者

`with_stream` 还有一个很实用的性质：消费端提前返回时，它会请求生产者停止，并**等生产者退出**才返回。看这个只读第一项的例子：

```cpp
faio::task<void> read_first_item() {
  std::atomic<bool> producer_stopped{false};
  const auto first = co_await faio::with_stream<int>(
      [&](faio::stream_producer<int>& output) -> faio::task<void> {
        try {
          co_await produce(output);
        } catch (const faio::operation_cancelled&) {
          producer_stopped.store(true);
          throw;
        }
      },
      [](faio::stream_reader<int>& input) -> faio::task<std::optional<int>> {
        co_return co_await input.next();  // 只取一项就返回
      });
  if (first != 1 || !producer_stopped.load())
    throw std::logic_error("提前退出后应等待生产者退出");
}
```

消费者读了一项就返回，生产端停在第二次 `emit()` 上。`with_stream` 请求它停止，它会抛出 `operation_cancelled`，在 catch 里记下标记。`with_stream` 等到标记设置完才返回，所以函数外面看到的 `producer_stopped` 一定是 `true`。

这个保证很重要：如果生产端借用了外部资源，消费端提前退出也不会让资源的使用悬在空中。

## 限制并发处理数

默认的处理方式是读一项、处理一项。想让几项同时处理，用 `for_each_concurrent`：

```cpp
std::atomic<int> sum{0};
co_await faio::for_each_concurrent<int>(
    2, produce, [&sum](int value) -> faio::task<void> {
      co_await faio::this_coro::yield();
      sum.fetch_add(value, std::memory_order_relaxed);
    });
// sum == 15
```

第一个参数 `2` 是并发上限：最多同时有两个处理任务在跑。`produce` 是流工厂，后面那个 lambda 是每项的处理器。`limit` 为 0 会抛 `std::invalid_argument`。

因为处理任务可能并行执行，修改共享状态要用原子操作或加锁——这里用 `sum.fetch_add()`。1 到 5 加起来是 15。

`for_each_concurrent` 还有一个重载，接收一个已经存在的 `stream_reader<T>&`，可以在 `with_stream` 的主体内部使用。

## 用在回显服务上

把这套模式套回我们的贯穿项目：假设服务端要逐行处理客户端发来的消息，限制同时处理 4 行，可以写一个按行产出字符串的流：

```cpp
faio::task<void> emit_lines(faio::stream_producer<std::string>& output,
                            faio::net::TcpStream& stream) {
  faio::io::BufReader reader{stream, 1024};
  std::string line;
  while (co_await faio::io::read_line(reader, line, 4096)) {
    co_await output.emit(line);
    line.clear();
  }
}

faio::task<void> serve_with_limit(faio::net::TcpStream stream) {
  co_await faio::for_each_concurrent<std::string>(
      4,
      [&](faio::stream_producer<std::string>& out) { return emit_lines(out, stream); },
      [](std::string line) -> faio::task<void> {
        // 处理这一行
        co_return;
      });
}
```

`stream` 要活过整个流操作——生产端只是借用它的引用。

## 完整代码

本章所有片段都来自 [`examples/coroutine/sequence.cpp`](../../examples/coroutine/sequence.cpp)，可以单独编译运行：

```sh
cmake --build build
./build/my_app
```

依次读取 1 到 5，只读取第一项，以及并发消费总和为 15，三种模式都会演示一遍。

## 教程到这里

我们从 `block_on()` 和 `sleep()` 开始，一步步走到了异步流。回头看，这套教程其实只用了不多的几个概念：

- `task<T>` 是惰性、移动独占的任务，用 `co_await` 或 `spawn()` 启动；
- `co_await` 让协程挂起，线程不阻塞；
- 组合子决定任务是全部完成、第一个完成，还是第一个成功；
- 同步原语和 I/O 都用 `expected` 表达失败，取消是协作式的；
- 结构化并发（`scope` / `task_group` / `with_stream`）保证退出时收尾干净。

拿着这些，你已经可以去读真实的 API 文档了。想看某个具体场景怎么落地，翻翻[专题](../topics/index.md)；想找某个类型的准确签名，看[API 介绍](../api/index.md)或[API 查询](../api-reference.md)。

---

[上一章：select 与并发组合](select.md) · [返回教程首页](../learn-faio.md)
