# 错误、诊断、取消与日志

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__errors.html)

faio 的异步接口用 `expected<T>` 返回结果，失败以 `Error` 值的形式出现，不通过业务异常传递。错误值记录原始代码、所属错误域、已完成进度，并可按需携带诊断上下文。直接使用这些类型时包含 `<faio/common/error.hpp>`，使用日志接口时包含 `<faio/log/logger.hpp>`。

```cpp
namespace faio {

enum class error_domain {
  faio,
  posix,
  resolver,
  win32,
  winsock,
  tls,
  process = 7,
};

class Error {
 public:
  enum ErrorCode {
    EmptySqe = 1000,
    InvalidAddresses,
    ClosedChannel,
    UnexpectedEOF,
    WriteZero,
    TooLongTime,
    PassedTime,
    InvalidSocketType,
    ReuniteFailed,
  };

  explicit Error(int err_code, std::uint64_t transferred = 0,
                 error_domain domain = error_domain::posix);

  std::uint64_t transferred() const noexcept;
  std::uint64_t progress() const noexcept;
  error_domain domain() const noexcept;
  std::int64_t native_code() const noexcept;
  const std::shared_ptr<const error_context>& context() const noexcept;
  Error with_context(std::shared_ptr<const error_context> details) const;
  Error with_progress(std::uint64_t bytes) const;
  int value() const noexcept;
  std::string_view message() const noexcept;
};

template <typename T>
using expected = std::expected<T, Error>;

auto make_error(int err) -> Error;

struct operation_cancelled : std::exception {
  const char* what() const noexcept override;
};

}  // namespace faio
```

## 结果类型

`faio::expected<T>` 是 `std::expected<T, faio::Error>` 的别名，`T` 可以为 `void`。先检查结果，再取值：

```cpp
const auto count = co_await stream.read(buffer);
if (!count) {
  faio::log::logger()->error("read failed: {} (error {})",
                             count.error().message(), count.error().value());
  co_return;
}
co_return count.value();
```

`make_error(errno)` 用 POSIX 域和零进度构造错误，适合把系统调用失败直接转成 `Error`。`std::format` 可以直接格式化 `Error`，输出形如 `Permission denied (error 13)`；格式说明符必须为空。错误值是普通对象，复制开销很小，诊断上下文通过 `shared_ptr` 共享。日志接口使用 spdlog 的格式化器，记录错误时传 `message()` 与 `value()`。

## 错误值与进度

`value()` 返回错误域内的原始整数码。`domain()` 决定这个码交给谁解释：POSIX 错误码就是 `errno`，解析器错误码要交给 `gai_strerror()`，TLS 与进程错误码使用各自的稳定枚举。`native_code()` 以 `int64_t` 返回同一个码，便于与平台 API 的宽类型比较。

`message()` 依次尝试：上下文自带的诊断消息、TLS 与进程域的默认文案、`gai_strerror()`、faio 自定义码的内置文案、最后 `strerror()`。因此只拿到 `value()` 不足以还原完整语义，跨域比较错误码也没有意义。

`transferred()` 与 `progress()` 返回同一个值，表示失败发生前已经生效的进度：传输按字节计，目录删除等操作按条目计。停止请求不会回滚已经完成的读写，进度也不会清零；`with_progress(bytes)` 用绝对累计值替换进度，保留错误域、代码和上下文，不是把 `bytes` 加到旧值上。文件复制、精确读写和目录递归删除会在失败时填充这个字段，TLS 写失败则带上已经接受的明文字节数。

faio 自定义错误码从 1000 开始，0 到 999 留给系统错误：

| 代码 | 含义 |
| --- | --- |
| `EmptySqe` | 提交时没有可用的 io_uring 提交队列条目 |
| `InvalidAddresses` | 没有可用于本次操作的有效地址 |
| `ClosedChannel` | 通道已经关闭 |
| `UnexpectedEOF` | 读取在预期长度之前遇到流末尾 |
| `WriteZero` | 仍有待写数据时底层写入返回 0 |
| `TooLongTime` | 时间参数超出支持范围 |
| `PassedTime` | 请求的截止时间已经过去 |
| `InvalidSocketType` | socket 类型与请求的操作不匹配 |
| `ReuniteFailed` | 尝试合并来自不同 socket 的读写半部 |

## 错误域

| 错误域 | 错误码来源 |
| --- | --- |
| `faio` | faio 自定义错误码，从 1000 开始 |
| `posix` | `errno` 定义的系统调用错误 |
| `resolver` | `getaddrinfo()` 返回的地址解析错误 |
| `win32` | Windows 系统接口错误 |
| `winsock` | Windows socket 接口错误 |
| `tls` | TLS 配置、握手、证书验证或记录层错误 |
| `process` | 子进程启动、等待、管道或输出捕获错误 |

`process` 显式定为 7，枚举顺序不应作为编号契约。把 `resolver` 错误码交给 `strerror()`，或者把 TLS 错误码当作 `errno`，都会得到无关的文本。

TLS 错误使用 `tls::errc` 中的十个稳定分类：`invalid_config`、`invalid_server_name`、`certificate_verify_failed`、`hostname_mismatch`、`handshake_failed`、`protocol_error`、`unexpected_eof`、`unread_application_data`、`closed`、`unsupported_backend`。构造错误时，`tls_error()` 会一次性取走当前线程的 OpenSSL reason 队列，把原因码按提取顺序放进 `native_reasons`；调用方提供证书验证结果时，它还会把该结果写进 `verify_code`。队列随后清空。因此排查 TLS 问题时先看 `message()`，再看 `native_reasons` 与 `verify_code`，不要只依赖 `value()`。

进程错误使用 `process::errc` 中的十个分类：`invalid_configuration`、`runtime_unavailable`、`process_limit_reached`、`launch_queue_full`、`runtime_shutdown`、`operation_in_progress`、`output_limit_exceeded`、`child_not_waitable`、`unsupported_stdio`、`unsupported_creation_flags`。启动与等待过程中遇到的系统调用失败保留原来的 POSIX 或 Windows 错误域，只有进程模块自己的失败才使用 `process` 域。输出捕获失败时，错误上下文会合并子进程标识、退出码、信号（POSIX）、已经保留的标准输出与标准错误前缀，以及两个方向的截断标记；默认预算为 stdout 16 MiB、stderr 16 MiB、合计 32 MiB，超过预算返回 `output_limit_exceeded`。

## 诊断上下文

`error_context` 保存可读消息和模块附加的诊断字段。复制 `Error` 时上下文指针共享，`with_context()` 返回的新错误也共享同一份数据；以 `shared_ptr<const error_context>` 访问时内容只读，需要在构造错误前填好。

| 字段 | 内容 |
| --- | --- |
| `message` | 可读诊断文本；存在时优先于按域生成的默认消息 |
| `native_reasons` | TLS 后端原因码，按提取顺序保存 |
| `verify_code` | 可选的证书验证结果码 |
| `cause` | 底层错误的可选副本，保留其域、进度与上下文 |
| `process_id` | 关联的子进程标识 |
| `process_exit_code` | 已经观察到的进程退出码 |
| `process_signal` | POSIX 下因信号退出时的信号编号 |
| `captured_stdout` / `captured_stderr` | 失败前保留的输出字节，可能包含 NUL |
| `stdout_truncated` / `stderr_truncated` | 对应输出是否因超过预算被截断 |

`context()` 返回借用的 `shared_ptr` 引用，`Error` 必须比使用处活得更久；需要长期保存时复制这个指针。`cause` 为嵌套失败预留，当前没有生产者赋值。捕获的两个输出字段是字节容器，不是以 NUL 结尾的字符串，按长度读取。

## 取消

`operation_cancelled` 是普通异常类型，`what()` 返回固定文本“协程等待操作已取消”。等待型操作在停止请求到达时抛出它：时间睡眠、信号量、屏障、latch、条件变量、MPSC 接收、异步生成器、串行域和 `for_each`。网络与文件 I/O 走另一条路径，取消作为 `ECANCELED` 错误值返回；精确读写会带上已经传输的字节数，调用者拿到的仍然是 `expected`。

取消异常会沿 `co_await` 向上传播；传播到任务边界时状态变成 `stopped`，仅当子任务确实记录了取消、并且停止令牌确实被请求时才如此。`value_or_throw()` 在纯停止场景重新抛出 `operation_cancelled`。需要清理资源的协程可以捕获这个异常，完成清理后再决定重新抛出还是返回错误；不捕获时由上层任务处理。

停止请求只改变等待结果，不会让任务提前交付半成品结果，任务仍要等各分支排空。使用 `spawn_detached()` 启动的分离任务必须自行处理异常，逃逸到根任务的异常会终止进程。

## 日志

`faio::log::logger()` 返回一个静态 logger 的 `const shared_ptr` 引用，名字为 `faio`，默认输出到 stderr，级别为 `info`，级别达到 `warn` 时立即刷新。默认格式包含时间、级别、logger 名和线程号。这个 logger 独立于 spdlog 的默认 logger 与全局配置，不会替换应用已经设置的 sink。

```cpp
faio::log::logger()->set_level(spdlog::level::debug);
faio::log::logger()->info("worker {} started", id);
```

logger 与 sink 都是线程安全的，多个 I/O 线程可以同时写入。需要自定义输出时，在启动阶段替换 sink 或调整 pattern，避免与正在写日志的线程并发修改格式。

启动失败、超时与输出捕获错误的完整处理见 [command.cpp](../../examples/process/command.cpp)。相关页面：[协程与任务](tasks.md)、[TLS](tls.md)、[进程](process.md)。
