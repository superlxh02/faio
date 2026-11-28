# 错误、诊断、取消与日志

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__errors.html)

网络、文件等返回 `expected<T>` 的接口通过 `error` 值报告操作失败，任务体抛出的异常沿等待链传播。错误类型在 [`<faio/common/error.hpp>`](../../include/faio/common/error.hpp)，日志接口在 `<faio/log/logger.hpp>`。处理两种错误通道的示例见[错误处理](../topics/errors.md)。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 结果类型 | `expected<T>` | `std::expected<T, error>` 的别名，`T` 可以为 `void` |
| | `make_error(int)` | 用 POSIX 域和零进度构造 `error` |
| | `error` | `value()`/`domain()`/`native_code()`/`message()`/`transferred()`/`progress()` |
| | `with_progress(bytes)` | 把进度设为绝对累计值，旧值不参与累加 |
| | `with_context(details)` | 附加诊断上下文，复制时共享同一份数据 |
| | `operation_cancelled` | 等待型操作被停止时抛出的普通异常 |
| 错误域与进度 | `error_domain` | 决定 `value()` 交给谁解释，跨域比较错误码没有意义 |
| | `progress()` / `transferred()` | 失败前已生效的进度：传输按字节，目录删除按条目 |
| | faio 自定义码 | 从 1000 开始，0 到 999 留给系统错误 |
| 诊断上下文 | `error_context` | `message`、`native_reasons`、`verify_code`、`captured_*`、`process_*`、truncated 标志 |
| | `context()` | 返回 `const shared_ptr<const error_context>&`；借用期间保留 `error`，复制指针可独立持有上下文 |
| 取消 | 等待型操作 | sleep、信号量、闩、屏障、条件变量、MPSC 接收、生成器、串行域和 `for_each` 抛 `operation_cancelled` |
| | 网络与文件 I/O | 取消作为 `ECANCELED` 错误值返回，精确读写带已传输字节数 |

## 结果分支

`co_await` 交付 `expected<T>` 后，可以用 `faio::match(result, faio::ok(fn), faio::err(fn))` 分别处理成功值和错误。`expected<void>` 的成功回调不接收参数。模式匹配入口是 `<faio/common/match.hpp>`，模块入口是 `import faio.common;`。

分支覆盖、守卫、返回类型以及引用和移动规则由[模式匹配 API](match.md)说明。结合读取、刷新和取消错误的用法见[用模式匹配处理结果](../topics/errors.md#用模式匹配处理结果)。

## 错误域

| `error_domain` 成员 | 错误码来源 |
| --- | --- |
| `FAIO` | faio 自定义错误码 |
| `POSIX` | `errno` 定义的系统调用错误；也是 `error` 构造函数的默认域 |
| `RESOLVER` | `getaddrinfo()` 返回的地址解析错误 |
| `WIN32_API` | Windows 系统接口错误 |
| `WINSOCK` | Windows socket 接口错误 |
| `TLS` | TLS 配置、握手、证书验证或记录层错误 |
| `PROCESS` | 子进程启动、等待、管道或输出捕获错误 |

`PROCESS` 显式定为 7，枚举顺序不作为编号契约。TLS 与进程错误各自使用 `tls::errc`、`process::errc` 中的稳定分类，并携带各自的诊断上下文。`message()` 优先返回上下文消息；没有上下文时，按错误域、faio 内置错误码或系统错误码生成消息。

判断错误时同时检查 `domain()` 和 `value()`。faio 自定义码也可能由使用默认域的接口构造，不应仅凭错误码名称推断错误域。

## 日志

| 接口 | 行为 |
| --- | --- |
| `faio::log::logger()` | 名为 `faio` 的 spdlog logger，默认 `info`、输出到 stderr，`warn` 级别立即刷新 |
| `set_level()` / 记录接口 | 与 spdlog 默认 logger 独立，不替换应用已设置的 sink |
| 自定义输出 | 在启动阶段替换 sink 或调整 pattern，避免与正在写日志的线程并发修改 |
