# 日志

异步程序里，很多任务交织在同一个线程上。一条普通的日志行只能告诉你"某处发生了某事"，却很难还原出"这是哪个任务、在哪个时间点、沿着哪条调用链走到这里的"。这一篇讲 faio 的日志接口，以及怎么在并发场景下记录有用的诊断信息。

## faio 的 logger

faio 内置一个基于 spdlog 的 logger，通过 `faio::log::logger()` 取得：

```cpp
#include <faio/log/logger.hpp>

faio::log::logger()->info("worker {} started", worker_id);
faio::log::logger()->error("parse address failed: {}", error.message());
```

它有几个特点：

- 是一个函数内静态单例，返回 `const std::shared_ptr<spdlog::logger>&`，不会每次调用都复制；
- 默认写到带颜色的 stderr，日志名是 `faio`；
- 默认级别是 `info`，`warn` 及以上自动 flush；
- 默认格式包含时间、级别、日志名、线程 ID：`[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] [thread %t] %v`。

接口就是 spdlog 的接口——`info()`、`debug()`、`warn()`、`error()`、`flush()`、`set_level()` 都能直接用。格式串也按 spdlog 的 `fmt` 语法写。

faio 刻意使用自己的 logger 而不是占用 spdlog 的默认 logger，这样你的应用仍然可以保留全局 logger 和自己的配置。

## 配置级别

在程序开始时设置级别，避免运行中被并发修改：

```cpp
int main(int argc, char** argv) {
  faio::log::logger()->set_level(spdlog::level::info);
  // ...
}
```

想完全关掉日志（比如跑基准测试）：

```cpp
faio::log::logger()->set_level(spdlog::level::off);
```

如果要在运行期间更换 sink，必须在其他线程开始记录之前完成。并发修改 sink 容器是不安全的。

## 记录错误信息

`Error::message()` 返回 `std::string_view`，可以直接交给格式化接口：

```cpp
const auto result = co_await stream.read(buffer);
if (!result) {
  const auto& error = result.error();
  faio::log::logger()->error("read: {} in domain {}, code {}",
                             error.message(),
                             static_cast<int>(error.domain()),
                             error.value());
  co_return;
}
```

`Error` 本身实现了 `std::formatter`，所以也可以直接格式化整个错误对象，输出形如 `"消息 (error 码)"`：

```cpp
faio::log::logger()->error("operation failed: {}", error);
```

涉及部分传输时，把 `transferred()` 也记下来，重试逻辑会用到：

```cpp
faio::log::logger()->warn("transfer stopped after {} bytes: {}",
                          error.transferred(), error.message());
```

## 并发场景下的可读性

多个任务在同一个线程上交错执行，是异步日志最难读的地方。几个实践建议：

**给任务加标识。** 在每个任务开头记一条带 ID 的日志，后续日志都带上这个 ID：

```cpp
faio::task<void> handle_connection(int id, faio::net::TcpStream stream) {
  faio::log::logger()->debug("connection {} accepted", id);
  // 后续日志都带 id
}
```

**用 `worker_id()` 区分执行位置。** 注意它反映的是本次恢复在哪个 worker 上，不是永久的线程身份——异步完成后可能换一个 worker：

```cpp
const auto worker = co_await faio::this_coro::worker_id();
faio::log::logger()->debug("step on worker {}", worker);
```

**记关键状态转换，不要记每一行。** 异步调用的嵌套很深，逐行日志会淹没真正有用的信息。集中在"接收到请求""开始等待 I/O""收到取消""完成收尾"这些节点上。

**调试时临时降低级别。** 库内部的 `worker.hpp` 用 `debug()` 记录 worker 的启动和停止。把 logger 调到 `debug` 就能看到这些内部事件，不用改库代码。

## 什么时候 flush

日志默认在非错误级别下不立即写盘。程序即将退出、或者你需要确保一条关键日志落盘时，显式调用：

```cpp
faio::log::logger()->flush();
```

`warn` 及以上会自动 flush，所以错误消息通常不用手动处理。基准测试里常见的做法是记录完配置后立刻 `flush()`，确保输出顺序和测试框架一致。

## 运行看看

仓库的基准测试里有真实用法，可以读 `benchmark/tcp/faio_tcp_benchmark.cpp`：它记录后端类型、监听地址，并在关键节点 flush。

## 下一步

最后一篇专题讲测试：怎么给这些协程代码写稳定的单元测试，怎么避免测试因为时序问题而随机失败。见[单元测试](testing.md)。

---

[上一章：错误处理](errors.md) · [下一章：单元测试 →](testing.md)
