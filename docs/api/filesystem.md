# 文件、目录与元数据

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__filesystem.html)

`faio::fs` 覆盖已打开文件的读写、路径级操作、属性查询和目录枚举。`file` 与 `open_options` 来自 `<faio/fs/file.hpp>`，按路径调用的函数和元数据类型来自 `<faio/fs/path.hpp>`，目录枚举使用 `<faio/fs/directory.hpp>`。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 文件对象 | `file::open(context, path, options)` | 按 `open_options` 打开；无 options 等价于只读打开 |
| | `file::create(context, path)` | 使用 `write + create + truncate` 创建文件 |
| | `open_options` | `read`/`write`/`append`/`truncate`/`create`/`create_new`；矛盾组合返回 `EINVAL` |
| | `read` / `write` | 允许短读短写；非空缓冲区读到 0 表示文件结束 |
| | `read_exact` / `write_all` | 分块完成；提前 EOF 或零进展分别返回 `UNEXPECTED_EOF`、`WRITE_ZERO`，并带进度 |
| | `read_at` / `write_at` | 显式偏移，不改逻辑游标；append 模式下 `write_at` 返回 `EINVAL` |
| | `seek` / `seek_from` | 返回新偏移；结果越界或落到起点之前返回 `EINVAL` |
| | `metadata` / `permissions` | 打开文件的属性快照与权限位 |
| | `set_len` / `set_permissions` | `ftruncate` / `fchmod` |
| | `sync_data` / `sync_all` / `flush` | 数据同步 / 连元数据一起同步 / 只等待写操作完成 |
| | `close` / `try_clone` / `native_handle` | 关闭并等在途操作 / `dup` 复制句柄 / 借用原生描述符 |
| 元数据与权限 | `file_metadata` | `len`/`is_file`/`is_dir`/`is_symlink`/`permissions`/`same_file`/`file_type`/`modified`/`accessed` |
| | `permissions` | `bits()`/`readonly()`/`set_readonly()`，包装 `std::filesystem::perms` |
| 路径操作 | `metadata` / `symlink_metadata` | 前者跟随符号链接，后者查询链接本身 |
| | `canonicalize` | 规范化绝对路径，解析符号链接并要求路径存在 |
| | `hard_link` / `symlink` / `rename` | 创建硬链接、符号链接或重命名 |
| | `remove_file` / `remove_dir` | 删除文件或空目录 |
| | `create_dir` / `create_dir_all` | 创建目录；权限参数只作用于新建目录 |
| | `dir_builder` | `recursive()` 与 `permissions()` 选项，`create()` 按选项创建 |
| | `remove_dir_all` | 返回删除条目数；选项含 `max_depth`、`max_state_bytes`、`batch_size` |
| | `copy` | 复制普通文件并返回字节数；目标截断，失败不回滚 |
| | `read` / `read_to_string` / `write` | 整体读写；`read` 默认上限 256 MiB，超限返回 `EFBIG` |
| 目录枚举 | `read_dir(context, path)` | 打开目录返回 `directory_reader`；省略 context 时使用当前上下文 |
| | `next_entry()` | 返回 `expected<optional<dir_entry>>`；空 optional 表示枚举结束 |
| | `dir_entry` | `file_name()`、`path()`、`metadata()`、`file_type()` |
| | `close()` | 等待在途批次结束并关闭目录句柄 |
