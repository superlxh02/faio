# 缓冲与流操作

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__io.html)

入口：`<faio/io.hpp>`。公开接口位于 `faio::io`；这些工具可以组合到文件、网络流及其他满足读写能力约束的对象上。

## io_buffer：拥有字节缓冲区

`io_buffer` 移动独占，区分可写容量 `capacity()` 与已经初始化的长度 `size()`。

| 调用 | 参数与结果 |
| --- | --- |
| `io_buffer(capacity)` | 分配指定字节数的可写空间；初始有效长度为 0 |
| `io_buffer::copy(bytes)` | 从 `span<const char>` 或 `string_view` 复制数据 |
| `buffer.bytes()` / `span()` | 取得已初始化范围的视图 |
| `buffer.writable_bytes()` | 取得全部可写空间；写入后通过 `set_size()` 发布长度 |
| `buffer.set_size(size)` | 标记已初始化长度；超过容量抛 `out_of_range` |
| `buffer.resize(size)` | 调整有效长度；增长的部分清零 |
| `buffer.reserve(capacity)` | 扩大容量，保留有效数据 |

```cpp
// 协程内，stream 为已经连接的 TcpStream。
auto payload = faio::io::io_buffer::copy(std::string_view{"hello"});
auto sent = co_await stream.write(std::move(payload));
if (!sent) co_return;
```

移动缓冲区后，异步操作完成前不要继续访问原对象。拥有型传输成功时通过 `io_transfer` 带回缓冲区和传输字节数。

## 完整传输算法

| 调用形态 | 完成结果 | 约定 |
| --- | --- | --- |
| `read_exact(reader, buffer)` | `expected<size_t>` | 继续短读直到填满，提前 EOF 返回错误 |
| `write_all(writer, buffer)` | `expected<void>` | 继续短写直到写完，零进度写入返回错误 |
| `write_all_buf(writer, buffer)` | `expected<io_transfer>` | 接管并在成功后归还拥有型缓冲区 |
| `read_to_end(reader, output, max_bytes)` | `expected<size_t>` | 追加至 `vector<char>`；可限制本次追加量 |

这些函数位于 `faio::io`，`reader` / `writer` 按引用传入；缓冲区和底层对象必须覆盖整个等待过程。错误前的传输进度通过 `Error::transferred()` 查询。

## BufReader 与 BufWriter

`BufReader<Reader>` 与 `BufWriter<Writer>` 借用底层对象，默认缓冲容量为 8192 字节，容量必须大于零。

| 接口 | 用途 |
| --- | --- |
| `reader.fill_buf()` | 异步获取尚未消费的缓冲视图 |
| `reader.consume(count)` | 确认消费指定长度；不能超过可用范围 |
| `reader.read(buffer)` | 从缓冲或底层读取，保留短读和 EOF 语义 |
| `reader.buffered()` | 查看当前未消费的预读内容 |
| `writer.write(buffer)` | 写入缓冲或底层，成功不等于已经持久化 |
| `writer.flush()` | 异步提交待写字节，失败时保留尚未写出的部分 |

使用预读适配器后，不要绕过它直接读取底层对象，否则会打乱未消费内容。`fill_buf()` 的视图在消费或后续读取后可能失效。写适配器析构不会执行异步 flush，退出前需要显式等待 `flush()` 或 `shutdown()`。

## Unix 原生描述符

Unix 平台提供 `faio::io::unix::AsyncFd`，适配可执行非阻塞操作的原生描述符。普通磁盘文件应使用 `faio::fs::File`。就绪提示仍需通过实际非阻塞调用确认，不能据此推断数据一定可读或可写。

[网络接口](network.md) · [文件接口](filesystem.md) · [文件操作例程](../examples/files.md)
