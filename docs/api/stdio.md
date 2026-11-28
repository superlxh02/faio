# 标准输入输出

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__stdio.html)

`faio::stdio` 提供可等待的标准流和格式化输出。格式化入口是 `<faio/stdio/print.hpp>`，标准流入口是 `<faio/stdio/stream.hpp>`，模块入口是 `import faio.stdio;`。

## 输出接口

| 接口 | 返回值 | 用法 |
| --- | --- | --- |
| `print(format, args...)` | `task<expected<void>>` | 写入标准输出 |
| `println(format, args...)` | `task<expected<void>>` | 写入标准输出并追加换行 |
| `eprint(format, args...)` | `task<expected<void>>` | 写入标准错误 |
| `eprintln(format, args...)` | `task<expected<void>>` | 写入标准错误并追加换行 |
| `print_to(writer, format, args...)`、`println_to(writer, format, args...)` | `task<expected<void>>` | 向现有异步 writer 输出，后者追加换行 |
| `format(format, args...)` | `string` | 同步生成采用相同适配规则的字符串 |
| `in()`、`standard_input()` | `stdin_stream` | 借用进程标准输入 |
| `out()`、`standard_output()` | `stdout_stream` | 借用进程标准输出 |
| `err()`、`standard_error()` | `stderr_stream` | 借用进程标准错误 |

```cpp
faio::task<faio::expected<void>> report(faio::expected<int> result) {
  auto prefix = co_await faio::stdio::print("result={}", result);
  if (!prefix)
    co_return std::unexpected{prefix.error()};
  co_return co_await faio::stdio::println(" optional={}", std::optional<int>{7});
}
```

格式串由 `format_string<Args...>` 在编译期校验。已有 `std::formatter` 的类型沿用原有格式说明；`expected`、`optional` 和 `variant` 可直接传入，文本摘要支持字符串宽度与对齐说明，faio 资源类型可输出诊断摘要。打印任务只观察对象，不消费任务结果。格式化在函数调用时完成，生成的字符串由待执行任务持有，因此临时格式化参数可在调用结束后销毁。格式化或内存分配抛出的异常在调用时传播，I/O 错误由等待后的 `expected` 返回。

同一输出通道的单次完整写入会串行执行，避免并发记录交错。输出失败时，错误中的已传输字节数反映本次调用已经写出的内容。

`print`、`println`、`eprint` 和 `eprintln` 提供首参数为 `io::operation_options` 的重载，可设置取消令牌与截止时间。`print_to` 和 `println_to` 借用 writer，writer 须存活到操作完成。

## 标准流

| 对象 | 操作 | 完成结果 |
| --- | --- | --- |
| `stdin_stream` | `read(span<char>, options)` | `expected<size_t>`；非空缓冲区读到零字节表示 EOF |
| `stdin_stream` | `read(io_buffer, options)` | `expected<io_transfer>`；归还拥有型缓冲区 |
| `stdin_stream` | `read_line(max_bytes, options)` | `expected<optional<string>>`；保留换行，EOF 且没有剩余内容时为空 |
| 输出流 | `write(span<const char>, options)` | `expected<size_t>`；允许短写 |
| 输出流 | `write(io_buffer, options)` | `expected<io_transfer>`；归还拥有型缓冲区 |
| 输出流 | `write_all(span<const char>, options)` | `expected<void>`；写完调用方保留的字节区间 |
| 输出流 | `write_all(string_view, options)` | `expected<void>`；调用时复制文本，任务持有副本并写完全部内容 |
| 输出流 | `write_all(string, options)` | `expected<void>`；右值移交字符串，左值复制后由任务持有 |
| 输出流 | `flush(options)` | `expected<void>`；刷新标准流缓冲 |

标准句柄由进程持有，流对象只借用它们。借用式 `read`、`write` 和 `write_all(span)` 的缓冲区须保留到等待完成，拥有式重载则把缓冲区移交给操作。`read_line` 和 `read` 共用输入缓冲，逐行读取后的预读字节仍能由后续读取取得。

`read_line` 的长度限制默认为 1 MiB，包含换行字节。达到限制仍未遇到换行时立即返回 `EMSGSIZE`，不再读取以确认 EOF，因此无换行的末行恰好达到限制也会报错。错误进度记录本次已消费的字节数，这段前缀不作为字符串返回；未消费的预读字节保留给后续读取。限制为零时返回 `EINVAL`。

Windows 真实控制台的 `print` 系列和 `write_all(string/string_view)` 使用 UTF-16 输出，重定向保留 UTF-8 字节；span 重载始终写入原始字节。Unix 断管道写入返回错误；Darwin 在写入期间临时启用标准描述符的 `NOSIGPIPE` 标志，最后一个活动写调用结束后恢复原值。

标准 I/O 在阻塞服务中执行，协程调度线程等待完成通知。取消和截止时间能阻止尚未开始的操作；原生阻塞调用已经开始时，操作会等待它安全结束后再返回取消或超时结果。等待通道互斥锁期间没有独立计时器，截止时间不保证准时唤醒。交互式输入可能因此继续等待用户输入。

无需交互输入的可运行示例见 [console.cpp](../../examples/stdio/console.cpp)，重定向及并发用例见 [test_stdio.cpp](../../tests/test_stdio.cpp)。
