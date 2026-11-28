# API 介绍

[打开公开 API 的 Doxygen 参考 →](../../api/index.html)

公开 API 按模块分页。模块页只列接口和功能；签名、参数和返回值查 Doxygen。TLS、异步进程和实验层不进 Doxygen。

## 模块

| 模块 | 内容 | 入口 |
| --- | --- | --- |
| 模式匹配 | expected、optional、variant 与值的分支处理 | [match](match.md) · [Doxygen](../../api/group__match.html) |
| 标准输入输出 | 协程标准流与格式化输出 | [stdio](stdio.md) · [Doxygen](../../api/group__stdio.html) |
| 协程与并发 | task、spawn、join、select、scope 与组合器 | [tasks](tasks.md) · [Doxygen](../../api/group__tasks.html) |
| 运行时与配置 | 调度模式、并发容量与生命周期 | [runtime](runtime.md) · [Doxygen](../../api/group__runtime.html) |
| 网络 I/O | TCP、UDP、地址解析与原生句柄 | [network](network.md) · [Doxygen](../../api/group__network.html) |
| 文件与目录 | 文件读写、元数据、路径与目录枚举 | [filesystem](filesystem.md) · [Doxygen](../../api/group__filesystem.html) |
| 时间与超时 | 休眠、周期定时器与 I/O 截止 | [time](time.md) · [Doxygen](../../api/group__time.html) |
| 协程同步 | 互斥锁、信号量、条件变量与 MPSC | [sync](sync.md) · [Doxygen](../../api/group__sync.html) |
| 缓冲与流操作 | 缓冲区、完整传输、适配器与内存流 | [io](io.md) · [Doxygen](../../api/group__io.html) |
| 错误与日志 | expected、error、错误域、取消与 logger | [errors](errors.md) · [Doxygen](../../api/group__errors.html) |
| TLS 加密 | 信任库、握手与加密流 | [tls](tls.md) |
| 异步进程 | 命令、标准流与回收 | [process](process.md) |
| 实验性接口 | 异步入口、生命周期注解与 Sender 互操作 | [experimental](experimental.md) |

## 包含哪个头文件

| 公共头文件 | 适用场景 |
| --- | --- |
| `<faio/common/match.hpp>` | 按成功、失败、存在、空值、类型或条件选择分支 |
| `<faio/stdio/print.hpp>` | 用格式串异步写入标准输出或标准错误 |
| `<faio/stdio/stream.hpp>` | 获取标准输入、输出和错误流 |
| `<faio/coroutine/task.hpp>` | 定义任务、查询当前协程、使用任务属性 |
| `<faio/coroutine/composition.hpp>` | 汇合、竞速、任务组、共享结果和串行执行域 |
| `<faio/coroutine/awaiter.hpp>` | 把应用自己的异步操作接入协程 |
| `<faio/coroutine/lifecycle.hpp>` | 清理、协作停止和任务截止 |
| `<faio/coroutine/sequence.hpp>` | 生成、消费和并发处理异步序列 |
| `<faio/runtime/default.hpp>` | 配置、驱动、提交任务和关闭默认运行时 |
| `<faio/runtime/instance.hpp>` | 创建独立运行时和跨实例执行任务 |
| `<faio/io/stream.hpp>` | 通用流算法、缓冲读写、拥有型缓冲区和内存流 |
| `<faio/io/native.hpp>` | 接入已有 Unix 文件描述符 |
| `<faio/net/address.hpp>` | 构造、解析地址和查询主机名 |
| `<faio/net/tcp.hpp>` | 建立、监听和读写 TCP 连接 |
| `<faio/net/udp.hpp>` | 创建 UDP 套接字并收发数据报 |
| `<faio/net/local.hpp>` | Unix 域套接字与本地管道 |
| `<faio/fs/file.hpp>` | 打开文件、读写、定位和同步 |
| `<faio/fs/directory.hpp>` | 逐条枚举目录 |
| `<faio/fs/path.hpp>` | 路径、元数据与路径级操作 |
| `<faio/sync/mutex.hpp>` | 互斥锁、RAII 锁与条件变量 |
| `<faio/sync/coordination.hpp>` | 信号量、闩与屏障 |
| `<faio/sync/channel.hpp>` | 任务间传送消息的 MPSC 通道 |
| `<faio/time/timer.hpp>` | 休眠、周期定时器和 I/O 超时 |
| `<faio/common/error.hpp>` | 错误与结果类型 |
| `<faio/log/logger.hpp>` | 获取日志记录器和调整日志级别 |
| `<faio/tls/handshake.hpp>` | 配置信任、发起或接纳握手 |
| `<faio/tls/stream.hpp>` | 读写已建立的 TLS 连接 |
| `<faio/process/command.hpp>` | 配置、启动命令、等待退出或捕获输出 |
| `<faio/process/child.hpp>` | 操作已有子进程的标准流 |

功能头同时提供签名用到的类型。需要全部稳定功能时包含 `<faio/faio.hpp>`；只包含上表用到的头可以控制编译开销。

## 组合器

| 主题 | 页面 |
| --- | --- |
| 汇总与限并发 | [join_settled](join_settled.md) · [join_all_settled](join_all_settled.md) · [join_all_limited](join_all_limited.md) |
| 竞速 | [select_success](select_success.md) · [race_in](race_in.md) |
| 终态与清理 | [settle](settle.md) · [with_cleanup](with_cleanup.md) · [with_stop_token](with_stop_token.md) · [complete_before](complete_before.md) |
| 任务组 | [task_group](task_group.md) |
| 序列与调度 | [with_stream](with_stream.md) · [for_each_concurrent](for_each_concurrent.md) · [on](on.md) · [serial_domain](serial_domain.md) · [shared_task](shared_task.md) |

结果类型 `task_outcome`、`group_result`、`select_result` 的字段见 [任务终态与结果类型](tasks.md#任务终态与结果类型)。

## 返回值

`task<T>` 在 `co_await` 之后交付 `T`。网络与文件接口用 `expected<T>` 表示成功或失败，先检查再取值。等待器类型不必显式写出，用 `auto` 接住操作。第一次使用先看 [快速开始](../getting-started.md)；错误、进度和取消规则见 [错误与日志](errors.md)。
