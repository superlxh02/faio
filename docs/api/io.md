# 缓冲与流操作

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__io.html)

文件、套接字、TLS 流和内存流都用 `read`/`write` 暴露字节。`faio::io` 在这些操作之上定义缓冲区所有权、完整传输、缓冲适配器和按行切分；主接口在 `<faio/io/stream.hpp>`，Unix 原生描述符在 `<faio/io/native.hpp>`。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 缓冲区 | `io_buffer` | 可增长字节存储；`copy()` 构造，`bytes()`/`writable_bytes()`/`set_size()`/`resize()`/`reserve()` 管理容量与长度；不可复制 |
| | `io_transfer` | 操作结束交回 `buffer` 与本次 `bytes` |
| | `borrowed_buffer` / `borrowed_const_buffer` | 借用调用方内存到操作完成，不延长源存储寿命 |
| | `shared_const_buffer` | 构造时复制，可多次发送共享同一份只读存储 |
| | `read_buf` | 记录存储与已填充长度；`filled()`/`unfilled()`/`advance()`/`clear()`；`TcpStream::read_buf` 读取成功后自动 advance |
| 完整传输 | `read_exact` | 读满整个 span；中途 EOF 返回 `UnexpectedEOF` 并带已读字节数 |
| | `write_all` / `write_all_buf` | 写完输入；零进展返回 `WriteZero`；buf 版本返还原缓冲区 |
| | `read_to_end` / `read_to_string` | 读到 EOF 或 `max_bytes`，返回本次追加的字节数 |
| | `copy` | 读到 EOF 后返回累计复制量；`buffer_size` 为 0 返回 `EINVAL` |
| | `copy_bidirectional` | 双向复制，返回两个方向的字节数；等两个方向都退出 |
| 缓冲适配器 | `BufReader` | 内部缓冲；`fill_buf()`/`consume()`/`read()`/`buffered()`/`get_ref()`；容量须大于 0 |
| | `BufWriter` | 缓冲小段写入；`write()`/`flush()`/`shutdown()`/`pending_bytes()`/`take_pending()`；析构不执行异步 flush |
| | `BufStream` | 组合独立读缓冲与写缓冲，转发 read/write/flush/shutdown |
| 按行与分隔符 | `read_until` / `read_line` | 读到分隔符或 EOF，结果包含分隔符；默认上限 1 MiB，超限返回 `EMSGSIZE` |
| | `Lines::next_line()` | 去掉行尾 `\n` 与其前面的 `\r`；EOF 返回 `std::nullopt` |
| | `split_by_delimiter` | 去掉分隔符；连续分隔符产生空段 |
| 内存与测试流 | `MemoryStream` | 在 `std::vector<char>` 上实现读写与 `seek()`，测试和回放可用 |
| | `empty()` / `repeat(byte)` / `sink()` | 空流、固定字节填充流、丢弃写入的 sink |
| | `duplex(capacity)` | 返回一对相连端点；缓冲空或满时挂起，容量为 0 抛出 `std::invalid_argument` |
| 运行上下文 | `io_context` | 可复制的 I/O 服务租约；`current()`/`valid()`/`stopped()`/`stop_token()`/`balanced_context()` |
| | `current_context()` | 等价于 `io_context::current()`，更短的调用形式 |
| Unix 描述符 | `OwnedFd` | POSIX 描述符独占所有者；`get()` 借用，`release()` 转移关闭责任 |
| | `AsyncFd` | 注册非阻塞描述符；`ready()`/`readable()`/`writable()`/`try_io()`/`into_inner()`；拒绝普通文件、目录和块设备 |
| | `AsyncFdReadyGuard` | 保存一次就绪快照；`ready()`/`try_io()`/`clear_ready()` |
| | `Interest` / `Ready` / `ShutdownBehavior` | 等待方向、就绪标志与关闭方向 |
