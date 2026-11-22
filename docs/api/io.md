# 缓冲与流操作

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__io.html)

文件、套接字、TLS 流和内存流都通过 `read`/`write` 暴露字节。faio 在这些基本操作之上提供缓冲区所有权包装、完整传输与复制、缓冲适配器，以及按行或分隔符切分的读取器。类型位于 `faio::io`，包含 `<faio/io/stream.hpp>` 即可使用；Unix 原生描述符的接口在 `<faio/io/native.hpp>`。

## 缓冲区与所有权

```cpp
namespace faio::io {

class io_buffer;
struct io_transfer;

struct borrowed_buffer;
struct borrowed_const_buffer;
class shared_const_buffer;

class read_buf;

}  // namespace faio::io
```

### io_buffer

`io_buffer` 管理一段可增长的字节存储，大小分为容量和已初始化长度。直接调用 `read`/`write` 时，调用方通常只需要 `std::span`；需要让异步操作接管缓冲区所有权时，才用 `io_buffer` 或 `io_transfer`。

```cpp
faio::io::io_buffer packet = faio::io::io_buffer::copy("PING\r\n");
auto sent = co_await stream.write(std::move(packet));
if (!sent) {
  co_return std::unexpected{sent.error()};
}
const std::size_t written = sent->bytes;
```

| 接口 | 行为 |
| --- | --- |
| `io_buffer(capacity)` | 预留容量，初始大小为 0 |
| `io_buffer::copy(span)` / `copy(string_view)` | 复制数据并把大小设为副本长度 |
| `data()` / `size()` / `capacity()` / `empty()` | 查看存储和当前大小 |
| `bytes()` | 只暴露已初始化区间；非 const 版本可原地修改这些字节 |
| `writable_bytes()` / `mutable_bytes()` | 暴露全部容量，用于先写后 `set_size()` |
| `set_size(n)` | 设置已初始化长度，超过容量抛出 `std::out_of_range` |
| `resize(n)` | 调整大小；扩容部分补零 |
| `reserve(n)` | 扩容并保留已初始化字节 |

`io_buffer` 不可复制。移动之后，源对象不再持有存储；传给异步操作的缓冲区由操作持有，结束时通过 `io_transfer` 交回。操作完成前不要通过其他途径访问同一段存储。

`io_transfer` 把操作结束后的缓冲区和实际字节数放在一起返回，成员为 `buffer` 和 `bytes`。网络和文件接口的 `read(io_buffer)`、`write(io_buffer)` 使用这个结果类型。

### 借用视图与共享缓冲区

有些接口不想接管所有权，只借用调用方内存到本次操作完成：

| 类型 | 用途与生命周期 |
| --- | --- |
| `borrowed_buffer` | 借用一段可写内存，用于读取；操作完成前保持有效 |
| `borrowed_const_buffer` | 借用一段只读内存，用于发送；操作完成前不得修改 |
| `shared_const_buffer` | 把数据复制进共享的只读存储，可被多次发送共同引用 |

```cpp
std::string text = "hello";
auto view = std::span<const char>{text};
co_await stream.write(faio::io::borrowed_const_buffer{view});

faio::io::shared_const_buffer greeting{"hello"};
co_await stream.write(greeting);
co_await stream.write(greeting);
```

借用视图不延长源内存的生命周期。异步操作挂起时，如果源字符串或数组先被销毁，缓冲区会悬空。`shared_const_buffer` 在构造时完成复制，后续发送共享同一份不可变存储，适合广播或重试。

### read_buf

`faio::io::read_buf` 是另一种借用方式：它把调用方的存储和已经填充的长度分开记录，网络流可以直接追加读取。

```cpp
std::array<char, 4096> storage{};
faio::io::read_buf incoming{storage};

auto n = co_await socket.read_buf(incoming);
if (!n) {
  co_return std::unexpected{n.error()};
}
const auto received = incoming.filled();
```

| 成员 | 行为 |
| --- | --- |
| `read_buf(storage, initialized = 0)` | 绑定存储；`initialized` 超过存储长度时抛出 `std::out_of_range` |
| `filled()` | 返回前 `size()` 个字节的只读视图 |
| `unfilled()` | 返回剩余可写区间 |
| `advance(n)` | 把已填充长度增加 `n`，越界抛出 `std::out_of_range` |
| `clear()` | 把已填充长度归零，保留存储绑定 |
| `size()` / `capacity()` | 当前已填充字节数和存储总容量 |

`read_buf` 不拥有存储，也不负责扩容。`TcpStream::read_buf(read_buf&)` 和 `try_read_buf` 会读取到 `unfilled()`，成功后自动 `advance()`；因此同一个对象可以连续追加，`filled()` 会包含历次读取的内容。处理完一段后调用 `clear()`，或者用 `size()` 记录游标，避免重复处理旧数据。

## 完整传输与复制

```cpp
template <borrowed_async_reader Reader>
task<expected<std::size_t>> read_exact(Reader& reader, std::span<char> buffer);

template <borrowed_async_writer Writer>
task<expected<void>> write_all(Writer& writer, std::span<const char> bytes);

template <borrowed_async_writer Writer>
task<expected<void>> write_all(Writer& writer, std::string_view bytes);

template <borrowed_async_writer Writer>
task<expected<io_transfer>> write_all_buf(Writer& writer, io_buffer buffer);

template <borrowed_async_reader Reader>
task<expected<std::size_t>> read_to_end(
    Reader& reader,
    std::vector<char>& output,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max());

template <borrowed_async_reader Reader>
task<expected<std::size_t>> read_to_string(
    Reader& reader,
    std::string& output,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max());

template <borrowed_async_reader Reader, borrowed_async_writer Writer>
task<expected<std::uint64_t>> copy(
    Reader& reader,
    Writer& writer,
    std::size_t buffer_size = 65536);

template <class A, class B>
task<expected<std::pair<std::uint64_t, std::uint64_t>>> copy_bidirectional(
    A& first,
    B& second,
    std::size_t buffer_size = 65536);
```

| 接口 | 结束条件与结果 |
| --- | --- |
| `read_exact` | 读满整个 span 才成功；中途 EOF 返回 `UnexpectedEOF`，并带上已读字节数 |
| `write_all` | 写完整段输入才成功；连续零进展返回 `WriteZero` |
| `write_all_buf` | 成功后返还原缓冲区和写入长度；失败时缓冲区随错误结果销毁 |
| `read_to_end` | 一直读到 EOF 或达到 `max_bytes`，返回总字节数 |
| `read_to_string` | 与 `read_to_end` 相同，追加到 `std::string` |
| `copy` | 读到 EOF 后返回累计复制量 |

这些算法会循环处理短读和短写。底层操作失败时，错误中的 `progress()` 或 `transferred()` 表示失败前已经完成的字节数，调用方可以据此判断是否重试以及从哪里继续。单次 `read` 返回 0 表示 EOF，错误通过 `expected` 的 `error()` 返回，两者在返回值上可以区分。

`read_to_end` 和 `read_to_string` 在达到上限时正常返回，不把剩余数据当成错误；需要继续读取时再次调用即可。`max_bytes` 统计的是本次调用追加的字节数，已有的输出内容不占用这个额度。

`copy` 使用固定大小的 `io_buffer`，`buffer_size` 为 0 时返回 `EINVAL`。它不负责关闭写端，代理或转发场景通常在读到 EOF 后自行半关闭。

`copy_bidirectional` 在两个端点上各启动一个复制方向，内部占用大约 `2 * buffer_size` 字节。任一方向返回错误时，函数请求另一个方向停止，等待两个方向都退出，再返回第一个非 `ECANCELED` 的错误；第二个方向启动失败时也会请求停止，并重新抛出启动异常。正常结束时，每个方向在读到 EOF 后调用写端的 `shutdown_write()`；写端没有这个成员时退回 `shutdown()`，能否保留反方向取决于该类型的默认关闭方向。

```cpp
auto moved = co_await faio::io::copy_bidirectional(client, upstream, 32768);
if (!moved) {
  co_return std::unexpected{moved.error()};
}
const auto [to_upstream, to_client] = *moved;
```

返回值的第一项是 `first` 到 `second` 的字节数，第二项是反方向。传入的两个对象都要同时满足可读和可写。

## 缓冲适配器

```cpp
template <borrowed_async_reader Reader>
class BufReader;

template <borrowed_async_writer Writer>
class BufWriter;

template <class Stream>
  requires borrowed_async_reader<Stream> && borrowed_async_writer<Stream>
class BufStream;
```

### BufReader

`BufReader` 在底层读取器前加一段内部缓冲，适合按小块解析协议或按行读取。构造时默认容量为 8192 字节，容量为 0 会抛出 `std::invalid_argument`。

| 成员 | 行为 |
| --- | --- |
| `fill_buf()` | 缓冲区为空时从底层读取一次，返回当前可读区间；EOF 时返回空 span |
| `consume(n)` | 丢弃前 `n` 个已缓冲字节，超过可读长度时抛出 `std::out_of_range` |
| `read(span)` | 先消费缓冲，再按需读取底层；可能短读 |
| `read(io_buffer)` | 最多读取到缓冲区容量，返回 `io_transfer` |
| `buffered()` | 当前已缓冲但尚未消费的字节 |
| `get_ref()` | 取回底层读取器引用 |

当输出空间不小于内部缓冲且当前没有缓存时，`read` 会绕过内部缓冲，直接把底层结果写进调用方内存，减少一次拷贝。这个优化不改变返回语义，调用方仍要准备接受短读。

`fill_buf()` 和 `buffered()` 返回的视图在下一次 `consume()` 或读取后失效。不要在多个协程中并发读取同一个适配器，也不要绕过适配器直接读底层，否则缓冲里的字节会丢失顺序。

### BufWriter

`BufWriter` 积累小段写入，减少系统调用；默认容量为 8192 字节，容量为 0 会抛出 `std::invalid_argument`。

| 成员 | 行为 |
| --- | --- |
| `write(span)` | 能放进缓冲时直接收下并返回输入长度；放不下时先 flush，再决定缓冲或直写 |
| `write(io_buffer)` | 同上，返回 `io_transfer` |
| `flush()` | 反复写底层，直到缓冲清空；失败时保留未写部分并报告进度 |
| `shutdown()` | 先 flush，再关闭底层写方向 |
| `pending_bytes()` | 尚未写到底层的字节视图 |
| `take_pending()` | 取走未写字节并清空缓冲 |
| `get_ref()` | 取回底层写入器引用 |

输入长度不小于内部容量时，`write` 会绕过缓冲直接写底层。底层可能只接受一部分，因此返回的字节数可以小于输入长度。调用方需要完整发送时，用 `write_all` 或循环处理剩余部分。

`flush` 失败时，缓冲保留仍未确认写出的部分，错误进度包含此前已经提交的字节数。`pending_bytes()` 的视图在下一次写入、flush 或 `take_pending()` 后失效。析构函数不执行异步 flush；不显式调用 `flush()` 或 `shutdown()`，缓冲中的数据会丢失。写入成功也只表示数据被缓冲或已被底层接受，不表示已经落盘。

```cpp
faio::io::BufWriter writer{stream, 4096};
auto written = co_await writer.write(std::span<const char>{"hello"});
if (!written) {
  co_return std::unexpected{written.error()};
}
auto flushed = co_await writer.flush();
if (!flushed) {
  co_return std::unexpected{flushed.error()};
}
```

`BufStream` 把一个同时可读可写的流组合成独立的读缓冲和写缓冲。构造参数依次为底层流、读容量和写容量，两个容量都不能为 0。它转发 `read`、`write`、`fill_buf`、`consume`、`flush` 和 `shutdown`，生命周期规则与单独使用 `BufReader`、`BufWriter` 相同。

## 按行与分隔符读取

```cpp
template <class Reader>
task<expected<std::size_t>> read_until(
    Reader& reader,
    char delimiter,
    std::vector<char>& output,
    std::size_t max_bytes = 1024 * 1024);

template <class Reader>
task<expected<std::size_t>> read_line(
    Reader& reader,
    std::string& output,
    std::size_t max_bytes = 1024 * 1024);

template <class Reader>
class Lines;

template <class Reader>
Lines<Reader> lines(Reader& reader, std::size_t max_bytes = 1024 * 1024);

template <class Reader>
class SplitByDelimiter;

template <class Reader>
SplitByDelimiter<Reader> split_by_delimiter(Reader& reader, char delimiter);
```

| 接口 | 分隔符处理 | 结束与上限 |
| --- | --- | --- |
| `read_until` | 遇到分隔符时结果包含它；EOF 前没有分隔符时保留已读内容 | 读到分隔符或 EOF，两种情况都算成功 |
| `read_line` | 结果包含行尾 `\n` | 等价于以 `\n` 调用 `read_until` |
| `Lines::next_line()` | 去掉 `\n`，再去掉其前面的 `\r` | EOF 返回 `std::nullopt` |
| `split_by_delimiter` | 去掉分隔符 | EOF 返回 `std::nullopt`；连续分隔符产生空段 |

`read_until` 和 `read_line` 都向已有输出追加内容，不会先清空。复用同一个字符串或向量时，先 `clear()`，否则上一轮的内容会留在前面。读取出错时，已经追加的字节仍保留在输出中。

`read_until` 优先通过 `fill_buf()`/`consume()` 消费缓冲，因此放在 `BufReader` 后面时不会把分隔符之后的数据读走；底层读取器没有这两个成员时，退化为一次读一个字节。后者在长行上会明显变慢，批量解析时优先套一层 `BufReader`。

`max_bytes` 是单次调用的上限。读到上限仍未遇到分隔符时，`read_until` 返回 `EMSGSIZE`，并带上已经追加的字节数。行读取器也把这个上限用于一行内容。`split_by_delimiter` 没有单独的上限参数，内部沿用 `read_until` 的默认 1 MiB；单个段超过这个长度会得到 `EMSGSIZE`。

```cpp
faio::io::BufReader reader{stream};
std::string line;
for (;;) {
  auto n = co_await faio::io::read_line(reader, line);
  if (!n) {
    co_return std::unexpected{n.error()};
  }
  if (*n == 0) {
    break;
  }
  // line 包含行尾 '\n'，处理完后清空再读下一行。
  line.clear();
}
```

`Lines` 把同样的读取过程包装成 `next_line()`，返回值是 `expected<optional<std::string>>`：外层错误表示读取失败，内层 `nullopt` 表示正常 EOF，空行则是一个空的 `std::string`。`split_by_delimiter` 在输入以分隔符开头、出现连续分隔符或以分隔符结尾时都会返回空段，解析时需要把这种空段和真正的数据区分开。

## 内存流、空流与双工通道

```cpp
class MemoryStream;

struct Empty;
struct Repeat;
struct Sink;

Empty empty() noexcept;
Repeat repeat(char value) noexcept;
Sink sink() noexcept;

class DuplexStream;
std::pair<DuplexStream, DuplexStream> duplex(std::size_t capacity = 65536);
```

### MemoryStream

`MemoryStream` 在 `std::vector<char>` 上实现同一套读写接口，适合解析器测试、回放报文和不需要真实文件或套接字的场景。它可以从 `std::vector<char>` 或 `std::string_view` 构造，构造后持有独立的字节存储。

| 成员 | 行为 |
| --- | --- |
| `read(span)` | 从共享游标读取；游标在末尾时返回 0 |
| `write(span)` | 在游标处覆盖或扩展存储，返回写入长度 |
| `read(io_buffer)` / `write(io_buffer)` | 返回 `io_transfer`，并设置实际字节数 |
| `seek(position)` | 把读写共用的游标移到绝对位置 |
| `bytes()` | 返回底层 `const std::vector<char>&` 视图 |
| `flush()` / `shutdown()` | 空操作，直接成功 |

读写共用一个游标，先写后读通常要 `co_await stream.seek(0)`，`seek` 也是协程操作。游标越过末尾后再写入，`std::vector` 会把中间的空隙补零。`bytes()` 不复制数据，视图在后续写入或流对象销毁后失效。`MemoryStream` 没有内部同步，多个协程共享同一个对象时要自行串行化。

```cpp
faio::io::MemoryStream input{"GET / HTTP/1.1\r\nHost: example\r\n\r\n"};
std::string request_line;
auto n = co_await faio::io::read_line(input, request_line);
if (!n) {
  co_return std::unexpected{n.error()};
}
```

### 空流、重复流与丢弃流

| 工厂 | 读取行为 | 写入行为 |
| --- | --- | --- |
| `empty()` | 总是返回 0，相当于立即 EOF | 不可写 |
| `repeat(byte)` | 用指定字节填满整个 span | 不可写 |
| `sink()` | 不可读 | 丢弃全部输入并返回输入长度 |

`repeat` 适合构造固定填充的测试输入，读取长度由调用方缓冲区决定。`sink` 适合测量读取吞吐或丢弃不需要的输出；传给 `copy` 前先保存成左值。

```cpp
auto discard = faio::io::sink();
auto copied = co_await faio::io::copy(reader, discard);
```

### DuplexStream 与 duplex()

`duplex(capacity)` 创建一对相互连接的端点，两个方向各有一条容量为 `capacity` 的缓冲。写入一个端点的数据由另一个端点读出，适合在进程内模拟套接字、测试协议状态机或连接两个流处理阶段。`capacity` 为 0 时抛出 `std::invalid_argument`。

```cpp
auto [left, right] = faio::io::duplex(4096);
auto sent = co_await faio::io::write_all(left, std::span<const char>{"ping"});
if (!sent) {
  co_return std::unexpected{sent.error()};
}
std::array<char, 4> storage{};
auto received = co_await faio::io::read_exact(right, storage);
```

读取在缓冲为空时挂起，直到对端写入或关闭写方向。对端调用 `shutdown_write()` 后，本端可以继续读完缓冲里的数据，之后 `read` 返回 0。写入在缓冲满时挂起；对端销毁或本端已经关闭写方向时返回 `EPIPE`。单次 `write` 可以只写入一部分，需要完整发送时用 `write_all`。

`shutdown_write()` 只关闭当前端点的写方向，`shutdown()` 是它的别名。没有单独的读关闭接口。端点析构时两个方向都会关闭，对端会在读完剩余数据后看到 EOF。等待中的读写被取消时返回 `ECANCELED`。

## 运行上下文

```cpp
class io_context {
 public:
  io_context() noexcept = default;

  static io_context current();

  bool valid() const noexcept;
  explicit operator bool() const noexcept;
  bool stopped() const noexcept;
  std::stop_token stop_token() const noexcept;

  io_context balanced_context() const noexcept;
};

io_context current_context();
```

`io_context` 是可复制的 I/O 服务租约，内部共享同一个 I/O 域。网络流、文件句柄和 `AsyncFd` 在创建时绑定上下文，之后即使协程迁移到其他线程，资源仍归原来的域管理。该类型不放在 `<faio/io/stream.hpp>` 中，网络或文件功能头会带出它；统一入口是 `<faio/faio.hpp>`。

| 接口 | 用途 |
| --- | --- |
| `current()` | 读取当前线程绑定的上下文；不在运行时线程上时返回空上下文 |
| `current_context()` | 与 `current()` 相同，作为更短的调用形式 |
| `valid()` / `operator bool` | 判断是否持有有效租约 |
| `stopped()` | 空上下文或所属域已停止时返回 true |
| `stop_token()` | 取得协作停止令牌，可传给需要感知停机的工作 |
| `balanced_context()` | 为尚未注册的新连接选择同一运行时的目标分片；全部分片停止时返回空上下文 |

```cpp
auto context = faio::io::current_context();
if (!context || context.stopped()) {
  co_return;
}
auto token = context.stop_token();
```

多数接口的 `io_context` 参数默认取 `current()`，在运行时启动的协程里直接调用即可。需要把新连接放到指定分片，或者从非运行时线程创建资源时，显式传入上下文。`statistics()`、`blocking()`、`cleanup()` 和 `resolver()` 面向运行时与扩展代码，普通流操作不需要直接调用。

## Unix 描述符与就绪观察

以下接口只在非 Windows 平台提供，位于 `faio::io::unix`。普通磁盘文件仍应使用 `fs::File`。

```cpp
namespace faio::io::unix {

class OwnedFd {
 public:
  explicit OwnedFd(int descriptor = -1) noexcept;
  ~OwnedFd();

  OwnedFd(OwnedFd&&) noexcept;
  OwnedFd& operator=(OwnedFd&&) noexcept;

  int get() const noexcept;
  int release() noexcept;
};

template <native_fd_owner Owner = OwnedFd>
class AsyncFd {
 public:
  explicit AsyncFd(Owner owner, io_context context = io_context::current());

  static expected<AsyncFd> create(io_context context, Owner owner);

  task<expected<AsyncFdReadyGuard>> ready(Interest interest);
  task<expected<AsyncFdReadyGuard>> readable();
  task<expected<AsyncFdReadyGuard>> writable();

  int native_handle() const noexcept;

  template <class F>
  auto try_io(Interest interest, F&& function);

  expected<Owner> into_inner();
};

class AsyncFdReadyGuard {
 public:
  Ready ready() const noexcept;
  void clear_ready() noexcept;

  template <class F>
  auto try_io(F&& function);
};

}  // namespace faio::io::unix
```

### OwnedFd

`OwnedFd` 是 POSIX 描述符的独占所有者，析构时最多关闭一次。移动构造和移动赋值转移关闭责任，移动后的源对象变为 -1。`get()` 只借用描述符，不转移关闭责任；`release()` 返回描述符并把对象置空，之后由调用方负责关闭。

`OwnedFd` 不检查描述符是否有效，也不改变其阻塞状态。把无效描述符交给 `AsyncFd` 会在注册阶段失败。

### AsyncFd

`AsyncFd` 把一个非阻塞描述符注册到 I/O 上下文，并提供可读、可写和组合方向的等待。构造时会调用 `fstat` 检查类型，普通文件、目录和块设备会被拒绝，抛出 `std::system_error` 且错误码为 `EINVAL`；随后设置 `O_NONBLOCK` 和 `FD_CLOEXEC`。描述符一旦交给 `AsyncFd`，关闭责任随之转移。

`create()` 是返回 `expected` 的构造方式：系统调用失败转换为错误码，内存不足返回 `ENOMEM`。`native_handle()` 只借出描述符，不转移关闭责任；`into_inner()` 把所有权转回 `Owner`，有在途操作时可能失败。

`ready(Interest)` 等待指定方向，`readable()` 和 `writable()` 是常用方向的快捷方式。就绪结果只是提示，真正读写仍要通过非阻塞系统调用确认。`try_io()` 在资源上调用回调并传入描述符，回调返回 `EAGAIN` 时会清除对应方向的就绪缓存；其他错误原样返回。

```cpp
auto async = faio::io::unix::AsyncFd<>::create(
    faio::io::current_context(),
    faio::io::unix::OwnedFd{read_fd});
if (!async) {
  co_return std::unexpected{async.error()};
}

auto guard = co_await async->readable();
if (!guard) {
  co_return std::unexpected{guard.error()};
}
auto n = guard->try_io([](int fd) {
  char byte{};
  return ::read(fd, &byte, 1);
});
```

`AsyncFdReadyGuard` 保存一次就绪观察的快照、方向和资源代际。`ready()` 返回这份快照，`try_io()` 执行同方向的非阻塞调用，`clear_ready()` 只清除本次观察到的那一代事件，不会抹掉随后到达的新事件。守卫本身不占用数据方向的执行权，也不把描述符变成阻塞模式。

普通文件、目录和块设备的就绪没有异步磁盘 I/O 的含义，`AsyncFd` 直接拒绝这些类型。管道、套接字、事件描述符等可设为非阻塞的对象才适合注册。

### Interest、Ready 与 ShutdownBehavior

```cpp
enum class Interest : std::uint32_t {
  none = 0,
  readable = 1,
  writable = 2,
  read_write = 3,
};

struct Ready {
  std::uint32_t flags;
  std::uint64_t generation;

  bool is_readable() const noexcept;
  bool is_writable() const noexcept;
  bool is_error() const noexcept;
  bool is_read_closed() const noexcept;
  bool is_write_closed() const noexcept;
};

enum class ShutdownBehavior {
  Read = SHUT_RD,
  Write = SHUT_WR,
  Both = SHUT_RDWR,
};
```

| 类型 | 含义 |
| --- | --- |
| `Interest::readable` | 观察读方向 |
| `Interest::writable` | 观察写方向 |
| `Interest::read_write` | 同时观察两个方向，也可用 `operator|` 组合 |
| `Ready::is_error()` | 内核在就绪通知中报告了错误 |
| `Ready::is_read_closed()` | 读方向收到 EOF 提示；真实读取仍可能先返回未消费的数据 |
| `Ready::is_write_closed()` | 写方向已关闭；对端仍可能继续发送数据 |
| `ShutdownBehavior::Read` / `Write` / `Both` | 请求关闭读方向、写方向或两个方向 |

就绪快照允许假就绪：描述符可能在等待返回后又被其他执行者读空，实际系统调用仍要处理 `EAGAIN`。只有真实的非阻塞系统调用返回 `EAGAIN` 时，缓存中的对应就绪提示才会被清除。

`TcpStream::shutdown()` 返回一个异步等待器，默认参数是关闭写方向；`UdpSocket::shutdown()` 的默认参数是关闭两个方向。`co_await` 之后才会提交关闭动作，结果通过 `expected` 返回。关闭只影响对应的传输方向，不关闭描述符本身；描述符的释放仍由流对象或 `OwnedFd` 负责。

## 失败、取消与生命周期

流操作统一用 `expected` 返回结果。EOF 用 `read` 返回 0 表示，错误放在 `error()` 中；完整传输算法会把底层错误转换成带进度的错误，便于判断重试位置。

取消是协作式的。等待就绪、双工通道读写和复制任务被取消时，通常返回 `ECANCELED`；`copy_bidirectional` 会等待两个方向都退出后再返回，避免留下仍在访问缓冲区的子任务。调用方不要假设取消后缓冲区立即可以复用，先等操作结果返回。

借用内存、`read_buf`、`io_buffer` 和缓冲适配器的视图都有明确有效期。异步操作挂起期间，保持底层存储和流对象存活，不要并发修改同一段内存。`BufWriter` 的析构不执行异步 flush，`AsyncFdReadyGuard` 的就绪状态也不保证下一次系统调用成功。

## 需要留意的边界

`BufReader`、`BufWriter`、`BufStream` 的缓冲容量必须大于 0，否则构造抛出 `std::invalid_argument`。`duplex()` 的容量同样不能为 0；`copy` 和 `copy_bidirectional` 的 `buffer_size` 为 0 时返回 `EINVAL`。

`read_until` 和 `read_line` 的默认上限是 1 MiB，超过上限且没有遇到分隔符时返回 `EMSGSIZE`。`split_by_delimiter` 没有独立的上限参数，单个段同样受 1 MiB 限制。

`BufReader` 只缓冲读取，`BufWriter` 只缓冲写入；`BufStream` 会同时占用读、写两份容量。`copy_bidirectional` 为两个方向各分配一个 `buffer_size` 的缓冲区，`duplex()` 为两个方向各分配 `capacity`。在连接数很多的场景里，这些容量会直接叠加到常驻内存。

完整的可运行示例见 [内存流与缓冲适配器](../../examples/io/stream.cpp)。相关接口：[网络](network.md) · [文件系统](filesystem.md) · [运行时](runtime.md)。
