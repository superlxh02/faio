# 错误处理

faio 有两条错误通道：I/O 操作通过 `expected<T>` 把失败当数据返回；协程体里抛出的异常沿等待链传播。这两条路什么时候用哪条、边界在哪里，是这一篇要讲清楚的事。

## 两条通道

先看一个读操作的典型写法：

```cpp
const auto count = co_await stream.read(buffer);
if (!count) {
  const auto& error = count.error();
  faio::log::logger()->error("read: {} (code {})",
                             error.message(), error.value());
  co_return;
}
const std::size_t bytes_read = *count;
```

`read()` 的失败不会抛异常，而是装进 `expected` 里返回。你必须先检查成功状态，再解引用。这是 faio I/O 接口的一致约定：

- I/O、文件、网络、通道操作返回 `faio::expected<T>`；
- 协程体里的逻辑错误（`throw std::invalid_argument(...)`）走异常；
- 取消在两种通道里都可能出现——I/O 通常报告取消错误，同步等待会抛 `operation_cancelled`。

## `expected<T>` 是什么

它就是标准类型的别名：

```cpp
template <typename T>
using expected = std::expected<T, Error>;
```

所以你可以用 `has_value()`、`operator bool`、`value()`、`error()` 这些标准接口。`expected<void>` 没有可解引用的业务值，成功时只表示"操作完成了"。

`co_await` 不会自动展开 `expected`，也不会把失败变成异常。`co_await stream.read(...)` 得到的是 `expected<size_t>`，不是 `size_t`。这是新手最容易忽略的一点。

## 认识 `Error`

`Error` 不只是错误消息，它还带着诊断信息：

| 方法 | 返回 | 用途 |
| --- | --- | --- |
| `message()` | `std::string_view` | 可读的错误描述 |
| `value()` | `int` | 错误域内的原始整数码 |
| `native_code()` | `std::int64_t` | 有符号的原生错误码 |
| `domain()` | `error_domain` | 错误来源的域 |
| `transferred()` / `progress()` | `std::uint64_t` | 失败前已生效的字节数 |

错误域有七个：`faio`、`posix`、`resolver`、`win32`、`winsock`、`tls`、`process`。**同一个数值在不同域里含义不同**，看错误码之前先看 `domain()`。解析器错误和 Windows 错误不能按 POSIX errno 解释。

`message()` 返回的是借用视图；如果错误带上下文，视图的生命周期受上下文对象约束。想长期保存消息，复制成 `std::string`。

`context()` 里可能还有更详细的诊断——TLS 的证书验证信息、底层原因、进程的标准输出等。它同样是借用的，用的时候保证错误对象还活着。

## 部分进度很重要

`transferred()` 值得单独拿出来说。一次 I/O 失败或被取消，可能已经有部分字节被读写了。系统调用的副作用**不会回滚**。

所以重试前要先看进度：

```cpp
if (!result) {
  const auto done = result.error().transferred();
  // 从 buffer + done 继续，而不是从头重来
}
```

不能假设"失败就什么都没发生"。这也是 `read_exact` 遇到提前 EOF 时会报错的原因——它可能已经读了几个字节，那些字节还在缓冲区里，需要你自己决定是否使用。

## 异常走另一条路

协程体里的异常会存进任务结果，在三个地方按原类型重抛：

- `co_await` 一个任务时；
- `co_await` 一个 `join_handle` 或 `join` 组合时；
- 普通线程上调用 `block_on()` 时。

写法就是标准的 `try/catch`：

```cpp
faio::task<int> divide(int denominator) {
  if (denominator == 0) throw std::invalid_argument("除数不能为零");
  co_return 84 / denominator;
}

faio::task<void> recover() {
  try {
    co_return co_await divide(0);
  } catch (const std::invalid_argument&) {
    co_return -1;
  }
}
```

`spawn()` 的那一刻不会抛——异常跟着任务走，等到你等待它的结果时才出现。把等待点放进 `try` 块里。

## `task<expected<T>>` 不是任务失败

这是个容易混淆的情况。如果任务返回 `expected<T>` 并且返回了失败值，任务本身仍然**正常返回**：

```cpp
faio::task<faio::expected<int>> fetch() {
  co_return faio::expected<int>{std::unexpected{error}};
}
```

`join` 会把这个 `expected` 原样放进结果集合，`scope` 不会因此触发异常取消。业务代码要逐项检查每个结果。想让失败影响整组，得自己判断后 `request_stop()`，或者抛出真正的异常。

## 组合子的异常边界

不同的组合子对异常的态度不一样，选之前要想清楚：

| 组合子 | 遇到失败怎么办 |
| --- | --- |
| `join` | 等所有分支退出，再按参数顺序重抛第一个异常 |
| `select` | 第一个完成的分支胜出，包括异常分支 |
| `select_success` | 忽略失败，取第一个成功；全失败时收集诊断 |
| `join_settled` / `settle` | 不重抛，保留每个任务的终态 |

想看全貌就用 `settled` 系列，把成功、失败、停止都留在结果里慢慢处理。具体接口见[select 与并发组合](../tutorial/select.md)。

## 常见错误码

`Error::ErrorCode` 里几个 faio 自定义的值：

- `UnexpectedEOF`：还没读满就遇到了流末尾；
- `WriteZero`：还有数据要写，底层却返回了 0；
- `ClosedChannel`：通道已关闭；
- `TooLongTime` / `PassedTime`：时间参数越界或时间点已过。

这些是 `faio` 域的码，和系统 errno 不在一个空间里。

## 运行看看

错误处理散落在各个示例里。`examples/net/tcp_echo.cpp` 里的 `checked()` 是一个简化的检查工具——真实项目里通常写成更完整的错误处理逻辑，而不是一概转成异常。

## 下一步

知道了错误怎么表达，下一篇看怎么把诊断信息记下来：并发任务交织在一起时，日志该怎么用才不会看晕。见[日志](logging.md)。

---

[上一章：优雅停机](shutdown.md) · [下一章：日志 →](logging.md)
