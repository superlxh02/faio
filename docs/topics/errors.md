# 错误处理

faio 的 I/O 失败通常通过 `expected<T>` 返回，协程体抛出的异常则沿等待链传播。调用方先等待操作，再用条件判断或 `faio::match` 处理返回值；等待点抛出的异常用 `try/catch` 处理。错误类型定义在 [`<faio/common/error.hpp>`](../../include/faio/common/error.hpp)。

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

`read()` 的 I/O 失败以 `expected` 返回。调用方先检查成功状态，再解引用。错误的交付形式取决于操作：

- I/O、文件、网络、通道操作返回 `faio::expected<T>`；
- 协程体里的逻辑错误（`throw std::invalid_argument(...)`）走异常；
- 取消在两种通道里都可能出现：I/O 通常报告取消错误，同步等待会抛 `operation_cancelled`。

## `expected<T>` 的取值

它就是标准类型的别名：

```cpp
template <typename T>
using expected = std::expected<T, error>;
```

可以用 `has_value()` 或 `operator bool` 检查状态，再通过 `operator*` 读取成功值，通过 `error()` 读取错误。`value()` 在失败时会抛出 `std::bad_expected_access<faio::error>`。`expected<void>` 没有业务值，成功时只表示操作完成。

`co_await stream.read(...)` 直接得到 `expected<size_t>`，需要先检查再取值。`co_await` 不会自动展开 `expected`，也不会把失败转成异常。

## 用模式匹配处理结果

需要同时处理成功和失败时，用 `faio::match` 为两种状态提供回调。包含 `<faio/common/match.hpp>`，或使用已经包含它的 `<faio/faio.hpp>`。下面的分支都返回 `void`，用于记录读取结果：

```cpp
const auto result = co_await stream.read(buffer);
faio::match(
    result,
    faio::ok([](std::size_t count) {
      faio::log::logger()->info("read {} bytes", count);
    }),
    faio::err([](const faio::error& error) {
      faio::log::logger()->error("read: {} (code {})",
                                 error.message(), error.value());
    }));
```

成功分支接收读取长度，失败分支接收 `faio::error`。两种状态都必须覆盖，各分支的返回类型必须完全一致。匹配规则和引用、移动语义见[模式匹配](../api/match.md)。

`expected<void>` 的成功回调不接收参数。例如，刷新操作只需判断是否完成：

```cpp
const auto flushed = co_await stream.flush();
faio::match(
    flushed,
    faio::ok([] { faio::log::logger()->info("flushed"); }),
    faio::err([](const faio::error& error) {
      faio::log::logger()->error("flush: {}", error.message());
    }));
```

需要区分错误原因时，可以给 `err` 加守卫。系统错误码宏来自 `<cerrno>`，判断前先检查错误域。对非空缓冲区的流式读取，可以这样区分结果：

```cpp
const char* state = faio::match(
    result,
    faio::ok([](std::size_t count) { return count == 0 ? "eof" : "data"; }),
    faio::err(
        [](const faio::error& error) {
          return error.domain() == faio::error_domain::POSIX
                 && error.value() == ECANCELED;
        },
        [] { return "cancelled"; }),
    faio::err([](const faio::error&) { return "failed"; }));
```

守卫可能返回 `false`，因此仍需最后一个不带守卫的 `err` 覆盖其余错误；只保留带守卫的错误分支会编译失败。这个示例把状态转换为文本，没有执行重试，是否重试还需检查错误的进度。

`match` 同步处理已经取得的值，不会自动等待任务，也不会把错误值转换为异常。回调抛出的异常直接传播。`optional` 的 `some`/`none` 分支以及嵌套结果的处理见[协程结果与嵌套状态](../api/match.md#协程结果与嵌套状态)。

## 认识 `error`

`error` 除了消息，还带诊断信息：

| 方法 | 返回 | 用途 |
| --- | --- | --- |
| `message()` | `std::string_view` | 可读的错误描述 |
| `value()` | `int` | 错误域内的原始整数码 |
| `native_code()` | `std::int64_t` | 有符号的原生错误码 |
| `domain()` | `error_domain` | 错误来源的域 |
| `transferred()` / `progress()` | `std::uint64_t` | 失败前已生效的进度，单位由操作决定 |

同一个数值在不同错误域里可能含义不同，判断错误码之前先检查 `domain()`。例如，上面的取消分支同时比较 `error_domain::POSIX` 和 `ECANCELED`。解析器错误和 Windows 错误不能按 POSIX errno 解释，完整的错误域列表见[错误 API](../api/errors.md#错误域)。

`message()` 返回的是借用视图；如果错误带上下文，视图的生命周期受上下文对象约束。想长期保存消息，复制成 `std::string`。

`context()` 返回错误对象持有的共享指针引用，可能包含 TLS 证书验证信息、底层原因和进程标准输出等诊断。借用这个引用时须保持错误对象有效；需要独立保存上下文时，复制共享指针。

## 部分进度

一次 I/O 失败或被取消，可能已经有部分字节被读写。`transferred()` 和 `progress()` 返回这些已生效的字节数，系统调用的副作用不会回滚。其他操作的进度单位由接口定义，例如目录树删除按条目计数。

重试前要检查已经完成的数量：

```cpp
if (!result) {
  const auto done = result.error().transferred();
  // 从 buffer + done 继续，而不是从头重来
}
```

`read_exact` 遇到提前 EOF 时会报错，已经读取的字节仍在缓冲区里，需要调用方决定是否使用。无论用条件判断还是 `match`，错误分支都应先检查进度，再决定如何继续。

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

faio::task<int> recover() {
  try {
    co_return co_await divide(0);
  } catch (const std::invalid_argument&) {
    co_return -1;
  }
}
```

任务体的异常在领取结果时传播，应把等待点放进 `try` 块。创建任务、提交任务或分配内存时发生的异常仍可能同步抛出。

## `task<expected<T>>` 的失败留在 `expected` 里

如果任务返回 `expected<T>` 并带回失败值，任务本身仍然**正常完成**：

```cpp
faio::task<faio::expected<int>> fetch(faio::error error) {
  co_return faio::expected<int>{std::unexpected{error}};
}
```

`join` 会把这个 `expected` 原样放进结果集合，`scope` 不会因此触发异常取消。业务代码仍需逐项检查结果，可以在领取结果后用 `match` 处理。需要让失败影响整组时，由业务代码显式请求停止，或者抛出异常。

## 组合子的异常边界

组合子按各自规则交付分支异常：

| 组合子 | 遇到失败怎么办 |
| --- | --- |
| `join` | 等所有分支退出，再按参数顺序重抛第一个异常 |
| `select` | 第一个完成的分支胜出，包括异常分支 |
| `select_success` | 忽略失败，取第一个成功；全失败时收集诊断 |
| `join_settled` / `settle` | 不重抛，保留每个任务的终态 |

`settled` 系列把成功、异常和停止保存在 `task_outcome` 中，这类结果通过 `status()` 区分，具体读取方式见[任务终态与结果类型](../api/tasks.md#任务终态与结果类型)。

## 常见错误码

`error::error_code` 里几个 faio 自定义的值：

- `UNEXPECTED_EOF`：还没读满就遇到了流末尾；
- `WRITE_ZERO`：还有数据要写，底层却返回了 0；
- `CLOSED_CHANNEL`：通道已关闭；
- `TOO_LONG_TIME` / `PASSED_TIME`：时间参数越界或时间点已过。

这些是 faio 自定义错误码，从 1000 开始。`make_error()` 和 `error` 构造函数的默认错误域是 `POSIX`，判断时以错误对象实际的 `domain()` 为准。

## 示例与接口

[`tcp_echo.cpp`](../../examples/net/tcp_echo.cpp) 使用条件判断处理网络错误，[`match.cpp`](../../examples/common/match.cpp) 演示结果与可选值的模式匹配。错误类型和取消接口见[错误 API](../api/errors.md)，完整匹配规则见[模式匹配 API](../api/match.md)。

[上一章：优雅停机](shutdown.md) · [专题首页](index.md)
