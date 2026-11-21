# 异步进程

配置和启动命令时包含 `<faio/process/command.hpp>`，操作已有子进程时包含 `<faio/process/child.hpp>`。接口位于 `faio::process`。CMake 开启 `FAIO_ENABLE_PROCESS=ON`，链接 `faio::process`；安装后使用 `find_package(faio CONFIG REQUIRED COMPONENTS process)`。组件不需要 OpenSSL。Linux 通过目标传播 `_GNU_SOURCE`，手动编译时必须在系统头之前传入 `-D_GNU_SOURCE`。进程平台基线为 Linux glibc 2.34、macOS 13、Windows 10。

## 配置与启动

`Command(path)` 仅保存配置。`arg` 和两个 `args` 重载逐项追加独立 argv，不拆分引号、不展开变量、不隐式运行 shell。`args_clear` 清参数；`env/envs/env_remove/env_clear` 编辑子环境；`current_dir/current_dir_clear` 编辑子工作目录。`get_*` 返回本命令的配置或借用视图。修改同一对象需要调用者同步。

`set_stdin/set_stdout/set_stderr` 接收可复制的 `Stdio`：

| 工厂 | 子进程绑定 |
| --- | --- |
| `Stdio::inherit()` | 继承相应父标准流，默认配置 |
| `Stdio::null()` | `/dev/null` 或 Windows `NUL` |
| `Stdio::piped()` | 创建异步父端与同步子端 |
| `Stdio::from_file(path, mode)` | 普通文件；stdin 用 `read`，输出用 `truncate/append` |

执行边界检查空路径、NUL、显式字符串 UTF-8、环境 key 和配置矛盾。`from_file` 每次启动重新打开，`truncate` 有实际文件副作用。stdout/stderr 不应各自 truncate 同一个文件。相对文件路径和带目录的相对 program 均相对有效子 cwd；单个文件名按有效子环境 PATH 查找。Windows 只补 `.exe`，不查 PATHEXT。

| 入口 | 返回值与执行时机 |
| --- | --- |
| `spawn(context)` | `expected<Child>`，当前调用线程同步执行 OS 创建，不等待退出 |
| `spawn_async(options, context)` | `task<expected<Child>>`，首次恢复将创建工作交给有界专用启动队列 |
| `status(options, context)` | 创建后关闭未取出的父管道，等待 `ExitStatus` |
| `output(capture, context)` | 强制两个输出为 pipe，并发收集后返回 `Output` |

异步方法在**调用时**复制命令、cwd、环境和 context，task 不借用原 Command。默认 context 也是调用时的 current context；在 runtime 外创建 task 要显式传入有效 context。`output` 不修改原命令，stdin 保留配置；若需要立即 EOF，显式使用 `Stdio::null()`。

## Child 与标准流

Child 和三个标准流均只能移动。移动源为空，执行方法报告 EBADF。`stdin_stream/stdout_stream/stderr_stream` 返回借用指针；`take_*` 返回拥有型 optional 并清空对应槽。`ChildStdin::write`、两个输出流的 `read` 均提供 span 与 `io_buffer` 重载，单次操作允许短 I/O。可直接组合 `io::write_all/read_exact/copy`。管道按字节传输，不转码、不换行。

`ChildStdin::flush` 没有用户态缓冲，只检查状态；`shutdown` 关闭父写端，使子进程收到 EOF。标准流 `close` 排空已接纳的 I/O 后关闭，析构只接管关闭责任。借用 span 必须保持到任务完成。

| Child 操作 | 语义 |
| --- | --- |
| `id()` | 尚未缓存退出时返回直接子 pid |
| `try_wait()` | 立即探测，不关闭 stdin；重复返回相同 terminal cache |
| `wait(options)` | 首次恢复关闭未 take 的 stdin，然后异步等待；保留两个输出 |
| `start_kill()` | SIGKILL / TerminateProcess 请求，不等待 |
| `kill(options)` | 请求终止并异步等退出，返回 `expected<void>` |
| `std::move(child).wait_with_output(capture)` | 调用时消耗 Child，首次恢复并发等待与收集 |

同一 Child 至多一个退出等待；第二个返回 `operation_in_progress`。取消 wait 不杀子进程，之后可以再次 wait/kill。已 take 的 stdin 由调用者关闭，wait 不会替调用者关闭它。`kill_on_drop` 默认 false；启用后丢弃公开 Child 会请求终止直接子进程。两种情况下服务都承担最终回收。终止不涵盖孙进程或整棵进程树。

## deadline、捕获与错误

`io::OperationOptions` 包含绝对 `steady_clock` deadline 和额外 `stop`；与当前任务 stop 合并。一个 deadline 贯穿异步启动、读、等待，逐块读取不重新计时。OS 创建 syscall 无通用强制取消，取消发生在创建过程中时，必须等待创建调用不再借用快照并清理未交付 Child，因此安全返回可能超过 deadline。

`CaptureOptions` 默认 stdout/stderr 各 16 MiB，总量 32 MiB，每块 32 KiB。块大小允许 1 至 1 MiB；三个上限允许 0。两个输出与退出等待并发推进，不能先 wait 再 drain。达到上限后最多再读 1 byte 判别 EOF；确有超限时返回 `output_limit_exceeded`，保存合法前缀并无条件请求终止直接子进程。`Error::context()` 包含 `captured_stdout/captured_stderr`、pid、已知退出结果和 truncated 标志；`progress()` 为已接纳前缀总字节数。

非零退出码仍是成功的 `ExitStatus/Output`。Unix 信号退出的 `code()` 为 nullopt，使用 `signal/core_dumped/raw_unix`；Windows 保存完整 uint32_t，包含 259 和高位退出码。`to_string()` 返回 `exit code: N` 或 `signal: N`。

孙进程可能继续持有输出写端，直接 Child 退出不意味着 pipe EOF。流式读取或收集此类程序应设置 deadline。runtime 停止不等待闲置子进程自然退出；回收服务继续保存原生身份，公开 Child 仍可同步 `try_wait/start_kill`。已有管道不会自动迁移到新 runtime。

Unix 扩展为 `arg0/process_group`；Windows 扩展为 `creation_flags/raw_handle`。flags 采用明确白名单，不接受挂起创建、调试器等未实现模式。`raw_handle` 仅借用，不能关闭。

写入子进程标准输入时，将 stdin 设为 `Stdio::piped()`，启动后通过 `take_stdin()` 取得写端。完整写入后等待 `shutdown()` 交付 EOF。两条命令组成流水线时，可以把上游的 `take_stdout()` 与下游的 `take_stdin()` 交给 `io::copy`，复制完成后关闭下游写端，再等两个进程退出。若子进程可能同时产生大量输出，输入与输出应并发处理，防止管道写满后双方互相等待。

完整程序见 [进程示例](../../examples/process/command.cpp)。用可执行文件路径启动 `process_command`，它会启动自身的 `--child MODE` 子角色，依次演示捕获输出和超时后终止、回收。
