# 文件、目录与元数据

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__filesystem.html)

`faio::fs` 把已打开文件的读写、路径级操作、属性查询和目录枚举放在同一命名空间。`File` 与 `OpenOptions` 来自 `<faio/fs/file.hpp>`，按路径调用的函数和元数据类型来自 `<faio/fs/path.hpp>`，逐条枚举目录使用 `<faio/fs/directory.hpp>`。所有异步接口返回 `task<expected<T>>`，失败值携带错误域与可选的诊断上下文。

类型和选项只列常用成员，函数签名按用途分散在后文各节，完整声明见 Doxygen 页面。

```cpp
namespace faio::fs {

class Permissions {
 public:
  explicit Permissions(std::filesystem::perms bits = std::filesystem::perms::unknown) noexcept;
  std::filesystem::perms bits() const noexcept;
  bool readonly() const noexcept;
  void set_readonly(bool enabled) noexcept;
};

class Metadata {
 public:
  std::uint64_t len() const noexcept;
  bool is_file() const noexcept;
  bool is_dir() const noexcept;
  bool is_symlink() const noexcept;
  Permissions permissions() const noexcept;
  bool same_file(const Metadata& other) const noexcept;
  std::filesystem::file_type file_type() const noexcept;
  std::chrono::system_clock::time_point modified() const noexcept;
  std::chrono::system_clock::time_point accessed() const noexcept;
};

class DirBuilder {
 public:
  DirBuilder& recursive(bool enabled = true) noexcept;
  DirBuilder& permissions(Permissions permissions) noexcept;
  auto create(io::io_context context, path name) const -> task<expected<void>>;
  auto create(path name) const -> task<expected<void>>;
};

struct recursive_remove_options {
  std::size_t max_depth{256};
  std::size_t max_state_bytes{1024 * 1024};
  std::size_t batch_size{64};
};

auto metadata(io::io_context context, path location) -> task<expected<Metadata>>;
auto symlink_metadata(io::io_context context, path location) -> task<expected<Metadata>>;
auto canonicalize(io::io_context context, path location) -> task<expected<path>>;
auto set_permissions(io::io_context context, path location, Permissions permissions)
    -> task<expected<void>>;
auto rename(io::io_context context, path source, path destination) -> task<expected<void>>;
auto remove_file(io::io_context context, path location) -> task<expected<void>>;
auto hard_link(io::io_context context, path source, path destination) -> task<expected<void>>;
auto symlink(io::io_context context, path source, path destination) -> task<expected<void>>;
auto create_dir(io::io_context context, path location,
                Permissions permissions = Permissions{std::filesystem::perms::all})
    -> task<expected<void>>;
auto create_dir_all(io::io_context context, path location,
                    Permissions permissions = Permissions{std::filesystem::perms::all})
    -> task<expected<void>>;
auto remove_dir(io::io_context context, path location) -> task<expected<void>>;
auto remove_dir_all(io::io_context context, path location,
                    recursive_remove_options options = {}) -> task<expected<std::uint64_t>>;
auto copy(io::io_context context, path source, path destination)
    -> task<expected<std::uint64_t>>;
auto read(io::io_context context, path location,
          std::size_t max_bytes = 256 * 1024 * 1024)
    -> task<expected<std::vector<char>>>;
auto read_to_string(io::io_context context, path location,
                    std::size_t max_bytes = 256 * 1024 * 1024)
    -> task<expected<std::string>>;
auto write(io::io_context context, path location, std::span<const char> contents)
    -> task<expected<void>>;

}  // namespace faio::fs
```

省略 `io::io_context` 的重载只在创建请求时读取 `io::io_context::current()`，后续执行、路径访问与析构都不依赖调用线程的 TLS。

## 文件对象

`File` 是移动独占的句柄，默认构造得到空句柄。`open(context, path, options)` 按任意 `OpenOptions` 打开，`open(context, path)` 等价于只读打开，`open(path, options)` 再补上默认上下文；`create(context, path)` 和 `create(path)` 使用 `write + create + truncate`。`OpenOptions::flags()` 会拒绝两种组合：既没有读也没有写，或者在不可写时请求 `truncate`、`create`、`create_new`，两种情况都返回 `EINVAL`。新建文件的默认创建权限是 `0644`，最终权限还受进程 umask 约束。

```cpp
auto options = faio::fs::OpenOptions{}.read().write().create();
auto file = co_await faio::fs::File::open(context, "data.txt", options);
if (!file) {
  co_return;
}
auto written = co_await file->write_all(std::span<const char>{"hello"});
if (!written) {
  co_return;
}
co_await file->close();
```

单次 `read()` 和 `write()` 允许短读短写：非空缓冲区读到 0 表示文件结束。普通读写与 `read_exact()`、`write_all()` 共享逻辑游标，由游标租约串行化；`read_at()`、`write_at()` 使用显式偏移，不改动游标，可以并行执行。append 模式下调用 `write_at()` 返回 `EINVAL`。`read_exact()` 与 `write_all()` 按 2 MiB 分块并在块间让出，提前遇到 EOF 或零进展写入分别转成 `UnexpectedEOF` 与 `WriteZero`，错误里的进度保留已经传输的字节数。向量读写接口会复制 `iovec` 表，表指向的内存仍需保持有效，直到操作排空。

`seek(seek_from)` 返回新的逻辑偏移；`seek_from::end(offset)` 以 `fstat` 或原生 `statx` 取得的文件长度为基准。结果超出 `off_t` 范围或落到起点之前时返回 `EINVAL`。`seek(uint64_t)` 是从文件开头计算的便捷重载。

`metadata()` 返回打开文件的属性快照，`permissions()` 只取权限位。`set_len()` 通过 `ftruncate` 扩展或截断内容，`set_permissions()` 在描述符上执行 `fchmod`。`sync_data()` 先等待已接受的写操作完成，再调用 `fdatasync`；`sync_all()` 连元数据一起同步；`flush()` 只等待写操作完成，不承诺数据已经落盘。

`close()` 停止接纳新操作，等待在途操作结束，然后关闭描述符；重复调用返回同一结果，关闭后继续读写得到 `EBADF`。`try_clone()` 通过 `dup` 复制句柄并共享 faio 逻辑游标，操作系统的文件偏移不因此成为独立副本。`native_handle()` 返回借用的原生描述符，关闭责任仍属于 `File`。析构执行异步清理，不隐式 `fsync`；移动对象不影响已经开始的操作。

## 元数据与权限

`Metadata` 是 `stat` 或 `statx` 结果的只读快照。Linux 上支持原生 `statx` 时，路径查询直接提交给 I/O 后端，否则落到阻塞池执行 `stat`、`lstat`。`len()` 返回字节数；`is_file()`、`is_dir()`、`is_symlink()` 区分三类常见对象；`permissions()` 只保留 `07777` 覆盖的权限位；`same_file()` 同时比较设备号与 inode，因此指向同一 inode 的两个路径会被识别为同一文件；`file_type()` 覆盖 regular、directory、symlink、fifo、socket、block、character，其余归为 unknown；`modified()` 与 `accessed()` 使用 `std::chrono::system_clock`，保留纳秒部分。

`Permissions` 包装 `std::filesystem::perms`。`readonly()` 在 owner、group、others 都没有写位时为真，`set_readonly(true)` 清除全部写位，`set_readonly(false)` 只补回 owner 写位。`bits()` 可以直接交给 `set_permissions()`，也可以与 `perms` 常量比较。

## 路径操作

路径函数与 `File` 成员使用相同的 `task<expected<T>>` 约定，下表按完成结果归类。

| 接口 | 完成结果与作用 |
| --- | --- |
| `metadata()` / `symlink_metadata()` | `Metadata`；前者跟随符号链接，后者查询链接本身 |
| `canonicalize()` | 规范化的绝对路径，解析符号链接并要求路径存在 |
| `hard_link()` / `symlink()` | `void`；创建硬链接或符号链接；硬链接要求源路径存在且目标路径尚不存在 |
| `rename()` / `remove_file()` / `remove_dir()` | `void`；`remove_dir()` 只能删除空目录 |
| `create_dir()` / `create_dir_all()` | `void`；权限参数只作用于新建目录 |
| `remove_dir_all()` | 删除的条目数；递归删除且不保证原子性 |
| `copy()` | 复制的字节数；源必须是普通文件，目标会被截断 |
| `read()` / `read_to_string()` / `write()` | 读取内容或写入字节；读取有上限，写入会创建或截断文件 |

`create_dir_all()` 逐段创建路径，每段之间让出执行权；空路径返回 `ENOENT`，段数超过 256 或前缀超过 1 MiB 返回 `ENAMETOOLONG`。遇到已存在的段时会查询元数据确认它是目录，不是目录则返回 `ENOTDIR`，已有目录的权限不会被改写。`DirBuilder` 提供同样的能力，`recursive` 默认为 `false`，默认权限为 `perms::all`；`create()` 根据 `recursive` 选择 `create_dir_all()` 或 `create_dir()`。

`remove_dir_all()` 用显式栈遍历目录，不跟随目录符号链接，也没有递归调用栈。它按 `batch_size` 分步推进，每步开始前检查停止请求与上下文状态，取消时返回带已删除条目数的 `ECANCELED`。三个选项在进入时校验：`max_depth` 为零、`batch_size` 为零或超过 4096、根路径超过 `max_state_bytes` 都返回 `EINVAL`；打开的目录层数达到 `max_depth` 时返回 `ELOOP`。根路径不存在视为已经删除，成功返回 0；根路径是普通文件时直接删除。内存中的路径与名称总量受 `max_state_bytes` 限制。整个过程不原子，失败或取消时已删除的条目不会恢复。

`canonicalize()` 在阻塞池调用 `std::filesystem::canonical`，返回解析符号链接后的绝对路径，路径不存在时失败。`hard_link()` 通过 `linkat` 创建另一个目录项，源路径必须存在，目标路径不能已经存在。`set_permissions()` 在阻塞池执行 `chmod`，修改的是路径当前指向的对象。

`copy()` 先打开源文件并确认它是普通文件，源不是普通文件时返回 `EINVAL`。目标以 `write + create` 打开，若与源指向同一文件则返回 `EINVAL`，否则先截断到零长度再复制。数据复制完成后把源权限写到目标，最后关闭两个句柄；传输开始后的失败会带已复制字节数，目标中已写入的部分不会回滚。

`read()` 最多读取 `max_bytes`，默认 256 MiB；读数恰好达到上限时会再探测一个字节，确认还有数据则返回 `EFBIG` 并带上已读数量。`read_to_string()` 使用同一套读取逻辑，再把字节转成字符串。`write()` 以创建和截断方式打开文件，完整写入后关闭；中途失败时文件保留已经写入的前缀。

## 目录枚举

`read_dir(context, path)` 打开目录并返回 `ReadDir`，`read_dir(path)` 使用当前上下文。`next_entry()` 的结果是 `expected<std::optional<DirEntry>>`：空的 optional 表示枚举结束，外层错误表示目录读取失败。关闭目录或上下文已经停止后再调用返回 `EBADF`；前一次 `next_entry()` 尚未完成时并发调用返回 `EBUSY`。实现按批读取，最多缓存 64 个条目或 64 KiB 名称，多数调用直接交付缓存。

`DirEntry::file_name()` 返回目录项最后一段名称，`path()` 返回拼好的完整路径。`metadata()` 跟随符号链接，等价于对完整路径调用 `fs::metadata()`；`file_type()` 优先采用 `dirent` 提供的 `d_type`，未知时异步执行 `lstat`。目录在枚举期间被外部修改时没有快照保证，顺序也由文件系统决定。`close()` 等待在途批次结束并关闭目录句柄，重复关闭返回同一结果。

## 失败、取消与代价

路径操作与文件读写把失败作为 `Error` 返回，`domain()` 指明错误码属于 POSIX、解析器、TLS 还是进程，`progress()` 给出失败前已经生效的进度：传输按字节计，删除按条目计。部分完成的路径操作不会回滚：`copy()` 已经写入的内容、`remove_dir_all()` 已经删除的条目都留在原处，调用者根据进度决定是否重试。

停止请求到达时，`read_exact()` 与 `write_all()` 把 `operation_cancelled` 转成 `ECANCELED`，并把已传输字节放进进度；`remove_dir_all()` 在推进过程中检查停止，返回带已删除数量的 `ECANCELED`。文件关闭或上下文停止后，普通文件操作与目录枚举返回 `EBADF`。

支持原生操作时，元数据查询走 `statx` 一类的系统调用；否则请求进入阻塞池。普通读写共用游标串行化，位置读写可以并发；`read_exact()`、`write_all()` 和 `copy()` 按块推进并在块间让出，`remove_dir_all()` 的目录深度、路径内存和每批步数分别受三个选项约束。

创建、写入、读取和枚举的完整示例见 [file_and_directory.cpp](../../examples/fs/file_and_directory.cpp)。相关接口：[协程与任务](tasks.md)、[I/O 与缓冲区](io.md)。
