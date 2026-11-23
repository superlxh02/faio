# 错误、诊断、取消与日志

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__errors.html)

faio 的异步接口以 `expected<T>` 返回结果，失败是一个 `Error` 值，不抛业务异常。类型在 `<faio/common/error.hpp>`，日志接口在 `<faio/log/logger.hpp>`。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 结果类型 | `expected<T>` | `std::expected<T, Error>` 的别名，`T` 可以为 `void` |
| | `make_error(int)` | 用 POSIX 域和零进度构造 `Error` |
| | `Error` | `value()`/`domain()`/`native_code()`/`message()`/`transferred()`/`progress()` |
| | `with_progress(bytes)` | 把进度设为绝对累计值，旧值不参与累加 |
| | `with_context(details)` | 附加诊断上下文，复制时共享同一份数据 |
| | `operation_cancelled` | 等待型操作被停止时抛出的普通异常 |
| 错误域与进度 | `error_domain` | 决定 `value()` 交给谁解释，跨域比较错误码没有意义 |
| | `progress()` / `transferred()` | 失败前已生效的进度：传输按字节，目录删除按条目 |
| | faio 自定义码 | 从 1000 开始，0 到 999 留给系统错误 |
| 诊断上下文 | `error_context` | `message`、`native_reasons`、`verify_code`、`captured_*`、`process_*`、truncated 标志 |
| | `context()` | 返回借用的 `shared_ptr`；`Error` 必须比使用处活得更久 |
| 取消 | 等待型操作 | sleep、信号量、闩、屏障、条件变量、MPSC 接收、生成器、串行域和 `for_each` 抛 `operation_cancelled` |
| | 网络与文件 I/O | 取消作为 `ECANCELED` 错误值返回，精确读写带已传输字节数 |

## 错误域

| 错误域 | 错误码来源 |
| --- | --- |
| `faio` | faio 自定义错误码 |
| `posix` | `errno` 定义的系统调用错误 |
| `resolver` | `getaddrinfo()` 返回的地址解析错误 |
| `win32` | Windows 系统接口错误 |
| `winsock` | Windows socket 接口错误 |
| `tls` | TLS 配置、握手、证书验证或记录层错误 |
| `process` | 子进程启动、等待、管道或输出捕获错误 |

`process` 显式定为 7，枚举顺序不作为编号契约。TLS 与进程错误各自使用 `tls::errc`、`process::errc` 中的稳定分类，并携带各自的诊断上下文。`message()` 依次尝试上下文消息、域默认文案、`gai_strerror()`、内置文案和 `strerror()`。

## 日志

| 接口 | 行为 |
| --- | --- |
| `faio::log::logger()` | 名为 `faio` 的 spdlog logger，默认 `info`、输出到 stderr，`warn` 级别立即刷新 |
| `set_level()` / 记录接口 | 与 spdlog 默认 logger 独立，不替换应用已设置的 sink |
| 自定义输出 | 在启动阶段替换 sink 或调整 pattern，避免与正在写日志的线程并发修改 |
