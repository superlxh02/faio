# 实验性接口

实验层要求 Linux + GCC 16.1.x + libstdc++ 16 + C++26。它由 `FAIO_ENABLE_EXPERIMENTAL` 单独开启，基础库用户可以保持关闭。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 公共头文件 | `<faio/experimental/async_main.h>` | 用注解声明异步程序入口 |
| | `<faio/experimental/lifecycle.h>` | 为协程函数附加生命周期策略 |
| | `<faio/experimental/execution.h>` | 与 stdexec Sender/Receiver 互操作 |
| | `<faio/experimental/runtime_options.h>` | 显式配置异步入口时可用的选项 |
| | `<faio/experimental/runtime.h>` | 管理需要显式控制的执行实例 |
| 异步入口 | `[[= faio::experimental::main(app)]]` | 在全局命名空间标注 `faio::task<int> app(int, char**)` |
| | `faio_configure_entry(target MODE ASYNC)` | 构建目标调用；命令行参数转交入口协程，`co_return` 整数作为进程退出状态 |
| | `runtime_options` | 入口默认采用库启动选项，需要时附加注解显式配置 |
| Sender 互操作 | `faio::experimental::as_sender(task)` | 转移惰性任务的所有权，接入 Sender 图 |
| | `faio::experimental::spawn_sender(sender)` | 提交图并返回可以等待一次的结果句柄 |

## 任务生命周期注解

`<faio/experimental/lifecycle.h>` 与目标 `faio::experimental_lifecycle` 提供 `[[=faio::experimental::lifecycle<Policy>{}]]`。真实协程在开始执行及退出时调用策略的 `before()` 与 `after(const task_exit&) noexcept`；只返回现成 task 的转发函数不应用此注解。该组件单独启用，要求同一程序中涉及任务的翻译单元统一使用 C++26 与 `FAIO_EXPERIMENTAL_TASK_LIFECYCLE=1`，并额外拥有一个辅助协程帧。退出状态和异常传播方式见 [协程与并发](tasks.md)。
