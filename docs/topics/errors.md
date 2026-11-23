# 错误处理

faio 有两条错误通道：I/O 操作通过 `expected<T>` 把失败当数据返回；协程体里抛出的异常沿等待链传播。调用方在不同位置看到的是不同的失败形式。

## 两条通道

一次读操作通常这样写：

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

`read()` 的失败以 `expected` 返回，不抛异常。调用方先检查成功状态，再解引用。faio 的 I/O 接口遵循同一套规则：

- I/O、文件、网络、通道操作返回 `faio::expected<T>`；
- 协程体里的逻辑错误（`throw std::invalid_argument(...)`）走异常；
- 取消在两种通道里都可能出现：I/O 通常报告取消错误，同步等待会抛 `operation_cancelled`。

## `expected<T>` 是什么

它就是标准类型的别名：

```cpp
template <typename T>
using expected = std::expected<T, Error>;
```

所以你可以用 `has_value()`、`operator bool`、`value()`、`error()` 这些标准接口。`expected<void>` 没有可解引用的业务值，成功时只表示"操作完成了"。

`co_await stream.read(...)` 直接得到 `expected<size_t>`，需要先检查再取值。`co_await` 不会自动展开 `expected`，也不会把失败转成异常。

## 认识 `Error`

`Error` 除了消息，还带诊断信息：

| 方法 | 返回 | 用途 |
| --- | --- | --- |
| `message()` | `std::string_view` | 可读的错误描述 |
| `value()` | `int` | 错误域内的原始整数码 |
| `native_code()` | `std::int64_t` | 有符号的原生错误码 |
| `domain()` | `error_domain` | 错误来源的域 |
| `transferred()` / `progress()` | `std::uint64_t` | 失败前已生效的字节数 |

错误域有七个：`faio`、`posix`、`resolver`、`win32`、`winsock`、`tls`、`process`。**同一个数值在不同域里含义不同**，看错误码之前先看 `domain()`。解析器错误和 Windows 错误不能按 POSIX errno 解释。

`message()` 返回的是借用视图；如果错误带上下文，视图的生命周期受上下文对象约束。想长期保存消息，复制成 `std::string`。

`context()` 里可能还有更详细的诊断：TLS 的证书验证信息、底层原因、进程的标准输出等。它同样是借用的，用的时候保证错误对象还活着。

## 部分进度

`transferred()` 值得单独拿出来说。一次 I/O 失败或被取消，可能已经有部分字节被读写了。系统调用的副作用**不会回滚**。

重试前要检查已经完成的数量：

```cpp
if (!result) {
  const auto done = result.error().transferred();
  // 从 buffer + done 继续，而不是从头重来
}
```

失败前已经发生的读写不会回滚，重试时不要从头覆盖这些字节。`read_exact` 遇到提前 EOF 时会报错，因为它可能已经读了几个字节，这些字节仍在缓冲区里，需要调用方决定是否使用。

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

`spawn()` 的那一刻不会抛。异常跟着任务走，等到你等待它的结果时才出现。把等待点放进 `try` 块里。

## `task<expected<T>>` 的失败留在 `expected` 里

如果任务返回 `expected<T>` 并带回失败值，任务本身仍然**正常完成**：

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

想看全貌就用 `settled` 系列，把成功、失败、停止都留在结果里慢慢处理。具体接口见[协程与并发](../api/tasks.md)。

## 常见错误码

`Error::ErrorCode` 里几个 faio 自定义的值：

- `UnexpectedEOF`：还没读满就遇到了流末尾；
- `WriteZero`：还有数据要写，底层却返回了 0；
- `ClosedChannel`：通道已关闭；
- `TooLongTime` / `PassedTime`：时间参数越界或时间点已过。

这些是 `faio` 域的码，和系统 errno 不在一个空间里。

## 运行看看

错误处理散落在各个示例里。`examples/net/tcp_echo.cpp` 在每个 `co_await` 之后直接检查 `expected`，把错误域和消息写进日志。

## 下一步

错误处理到这里告一段落。需要查具体接口时，看 [API 介绍](../api/index.md)；需要回到任务模型，可以从[专题首页](index.md)重新开始。


[上一章：优雅停机](shutdown.md) · [专题首页](index.md)
