# 异步进程

进程接口在 `faio::process`。命令配置和启动包含 `<faio/process/command.hpp>`，已有子进程包含 `<faio/process/child.hpp>`。CMake 打开 `FAIO_ENABLE_PROCESS=ON` 并链接 `faio::process`；安装后用 `find_package(faio CONFIG REQUIRED COMPONENTS process)`。组件不依赖 OpenSSL。Linux 上目标会传播 `_GNU_SOURCE`，手动编译要在系统头之前加 `-D_GNU_SOURCE`。平台基线：Linux glibc 2.34、macOS 13、Windows 10。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 命令配置 | `Command(path)` | 仅保存配置，启动时才解析 program |
| | `arg` / `args` / `args_clear` | 逐项追加 argv，不拆引号、不展开变量、不隐式运行 shell |
| | `env` / `envs` / `env_remove` / `env_clear` | 编辑子进程环境 |
| | `current_dir` / `current_dir_clear` | 编辑子进程工作目录 |
| | `get_*` | 读取本命令的配置或借用视图 |
| 标准流配置 | `set_stdin` / `set_stdout` / `set_stderr` | 接受可复制的 `Stdio` |
| | `Stdio::inherit()` / `null()` | 继承父标准流 / `/dev/null` 或 Windows `NUL` |
| | `Stdio::piped()` | 创建异步父端与同步子端 |
| | `Stdio::from_file(path, mode)` | 绑定普通文件；stdin 用 read，输出用 truncate 或 append |
| 启动 | `spawn(context)` | `expected<Child>`；当前调用线程同步执行 OS 创建 |
| | `spawn_async(options, context)` | `task<expected<Child>>`；创建工作交给有界专用启动队列 |
| | `status(options, context)` | 关闭未取出的父管道并等待 `ExitStatus` |
| | `output(capture, context)` | 强制两个输出为 pipe，并发收集后返回 `Output` |
| Child | `stdin_stream` / `stdout_stream` / `stderr_stream` | 返回借用的标准流指针 |
| | `take_stdin` / `take_stdout` / `take_stderr` | 返回拥有型 optional 并清空对应槽 |
| | `ChildStdin::write` / 输出流 `read` | 带 span 与 `io_buffer` 重载，允许短 I/O |
| | `ChildStdin::flush` / `shutdown` | 无用户态缓冲只检查状态 / 关闭父写端，使子进程收到 EOF |
| | `id()` / `try_wait()` / `wait(options)` | 直接子 pid / 立即探测 / 异步等待并缓存退出状态 |
| | `start_kill()` / `kill(options)` | 请求 SIGKILL 或 TerminateProcess / 请求并异步等待退出 |
| | `wait_with_output(capture)` | 消耗 Child，并发等待退出并收集输出 |
| | `kill_on_drop` | 丢弃 Child 时请求终止直接子进程，默认 false |
| 捕获 | `CaptureOptions` | 默认 stdout/stderr 各 16 MiB、总量 32 MiB、块 32 KiB；超限返回 `output_limit_exceeded` |
