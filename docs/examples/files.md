# 文件与目录操作

这个程序演示一个完整文件流程：创建目录，写入 C 源码，枚举目录，改变工作目录，再读出内容。

## 构建与运行

```sh
cmake --build build/local --target file_and_directory
mkdir -p build/file-demo
cd build/file-demo
../local/examples/file_and_directory
```

程序会在启动工作目录中创建 `test/main.c`，写入含有 `Hello, world!` 的 C 程序。重复运行会覆盖该文件，结束后保留生成内容。这里选择 `build/file-demo` 作为工作目录，使生成文件留在构建目录内。

## 阅读重点

1. `create_dir_all()` 对已存在目录可以成功。
2. `File::create()` 创建 / 截断文件，`write_all()` 负责处理短写。
3. 显式等待关闭之后，再继续后续操作。
4. `ReadDir::next_entry()` 的外层 `expected` 表示失败，内层空 `optional` 表示结束。
5. 改变工作目录使用标准库 `current_path()`；它是进程共享状态，示例按顺序执行。

## 完整源码

```cpp
{{#include ../../examples/file_and_directory.cpp}}
```

[API：文件系统](../api/filesystem.md) · [底层 I/O](../api/io.md)
