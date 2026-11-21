# 错误与日志

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__errors.html)

直接使用 `Error` 或 `expected<T>` 时包含 `<faio/common/error.hpp>`，使用日志时包含 `<faio/log/logger.hpp>`。

## expected 结果

`faio::expected<T>` 是 `std::expected<T, faio::Error>` 的别名。检查结果成功之后再取值：

```cpp
const auto count = co_await stream.read(buffer);
if (!count) {
  const auto& error = count.error();
  faio::log::logger()->error("read: {} (code {})", error.message(), error.value());
  co_return;
}
const std::size_t bytes_read = *count;
```

`expected<void>` 也需要检查成功状态，它没有可解引用的业务值。

## Error

| 方法 | 结果与用途 |
| --- | --- |
| `message()` | 错误说明的 `string_view` |
| `value()` | 错误码，返回 `int` |
| `native_code()` | 原生错误码，返回 `int64_t` |
| `domain()` | 错误来源的 `error_domain` |
| `transferred()` / `progress()` | 失败之前已经生效的传输字节数 |

`error_domain` 包括 `faio`、`posix`、`resolver`、`win32`、`winsock`、`tls`、`process`。解析器或 Windows 错误不能统一按 POSIX errno 解释。

`context()` 借用可选的共享诊断，包含TLS验证信息、底层原因或进程输出；视图和引用的使用期间须保持错误对象存活。`with_context()`、`with_progress()` 返回更新后的错误副本。

常见 faio 错误包括 `Error::UnexpectedEOF`、`WriteZero`、`ClosedChannel`。失败或取消可能已经产生部分副作用，重试之前应先检查进度。

## 协程异常

任务中抛出的异常通过 `co_await`、结果句柄或 `block_on()` 传播。同步原语和时间等待可能通过异常报告取消；使用 `spawn_detached()` 时，任务内部应处理异常。

任务停止为协作式请求，不会强制中断任意同步函数。组合任务会在返回或传播错误前等待已启动分支收束。

## 日志

`faio::log::logger()` 返回独立的 spdlog logger，默认 `info` 级别，输出到 stderr。

```cpp
faio::log::logger()->set_level(spdlog::level::debug);
faio::log::logger()->info("server listening on {}", 8080);
```

调整输出 sink 时应在并发记录日志之前完成配置；不要并发修改 sink 容器。

[协程接口](tasks.md) · [运行时配置](runtime.md)
