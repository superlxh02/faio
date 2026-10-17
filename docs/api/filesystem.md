# 文件与目录

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__filesystem.html)

入口：`<faio/fs.hpp>`。类型与路径操作位于 `faio::fs`。

## 文件句柄

| 接口 | 完成结果 / 用途 |
| --- | --- |
| `File::open(path)` | `expected<File>`，打开已有文件 |
| `File::create(path)` | `expected<File>`，创建可写文件；已有文件被截断 |
| `File::open(context, path, options)` | 显式指定 I/O 上下文与打开选项 |
| `file.read(buffer)` / `write(buffer)` | 读写并推进文件游标，可能短读 / 短写 |
| `file.read_exact(buffer)` / `write_all(buffer)` | 完整传输或报告错误 |
| `file.read_at(buffer, offset)` / `write_at(buffer, offset)` | 使用显式偏移进行位置 I/O |
| `file.seek(seek_from)` | 调整逻辑游标 |
| `file.metadata()` | 查询文件元数据 |
| `file.sync_all()` / `sync_data()` | 请求持久化同步；语义取决于平台与文件系统 |
| `file.close()` | 等待关闭，完成在途操作的收束 |

`OpenOptions` 通过 `read(bool)`、`write(bool)`、`append(bool)`、`create(bool)`、`create_new(bool)`、`truncate(bool)` 组合打开模式。`create_new()` 在文件已存在时失败；创建或截断需要启用写入。

```cpp
auto options = faio::fs::OpenOptions{}.read().write().create();
auto opened = co_await faio::fs::File::open("data.txt", options);
if (!opened) co_return;
```

带游标操作与显式偏移操作有不同的并发约定；共享一个文件对象时，应协调带游标的读写。

## 路径操作

| 接口 | 用途 |
| --- | --- |
| `create_dir()` / `create_dir_all()` | 创建单层 / 多层目录 |
| `metadata()` / `symlink_metadata()` | 查询元数据，决定是否跟随符号链接 |
| `read()` / `read_to_string()` / `write()` | 整个文件的便捷操作 |
| `copy()` / `rename()` | 复制 / 重命名 |
| `remove_file()` / `remove_dir()` / `remove_dir_all()` | 删除文件、空目录或目录树 |

这些接口通常返回 `task<expected<T>>`，便利重载会选择默认上下文。删除或复制可能已经产生部分副作用；失败时需要检查错误和进度后再决定重试。

## 逐条枚举目录

`read_dir(path)` 返回 `expected<ReadDir>`。对 `directory.next_entry()` 执行等待后得到 `expected<optional<DirEntry>>`：

- 外层失败：枚举出错，应先处理 `error()`。
- 外层成功、内层为空：枚举结束。
- 内层存在：读取 `file_name()`、`path()` 或异步 `file_type()` 等条目属性。

不要把枚举结束误当作 I/O 失败。枚举顺序由文件系统决定，不保证有序；完成后可显式等待 `directory.close()`。

## 生命周期与关闭

文件句柄移动独占，传入读写操作的借用缓冲区必须存活到操作完成。需要明确的关闭结果时执行 `co_await file.close()` 并检查返回值；不要把提交关闭当作已经完成关闭。

完整程序：[文件与目录例程](../examples/files.md)。
