# 实验性接口

实验层要求 Linux + GCC 16.1.x + libstdc++ 16 + C++26，通过 `FAIO_ENABLE_EXPERIMENTAL` 独立启用。基础库用户可以保持此开关关闭。

## 公共头文件

| 头文件 | 用途 |
| --- | --- |
| `<faio/experimental/async_main.h>` | 通过注解声明异步程序入口 |
| `<faio/experimental/lifecycle.h>` | 为协程函数附加生命周期策略 |
| `<faio/experimental/execution.h>` | 与 stdexec Sender/Receiver 互操作 |
| `<faio/experimental/runtime_options.h>` | 显式配置异步入口时可用的选项 |
| `<faio/experimental/runtime.h>` | 管理需要显式控制的执行实例 |

## 异步入口

在全局命名空间用 `[[= faio::experimental::main(app)]]` 标注 `faio::task<int> app(int, char**)`。构建目标调用 `faio_configure_entry(target MODE ASYNC)`。库将命令行参数转交给入口协程，并把 `co_return` 的整数用作进程退出状态。

入口默认采用库的启动选项。入口返回时，库请求派生任务停止并等待它们退出。应用需要显式配置时，可以附加 `runtime_options` 注解。[退出收尾示例](../../examples/experimental/entry/exit_cleanup.cpp)演示了默认退出策略如何等待后台清理完成。

完整例程直接引用仓库源码：

```cpp
{{#include ../../examples/experimental/entry/async_main.cpp}}
```

## Sender/Receiver 互操作

在 faio 协程中使用以下操作。库从调用位置取得执行上下文，沿用当前任务的调度与停止关系。

| 调用 | 用途 |
| --- | --- |
| `faio::experimental::as_sender(task)` | 转移惰性任务的所有权，将它接入 Sender 图 |
| `faio::experimental::spawn_sender(sender)` | 提交图并返回可以等待一次的结果句柄 |

等待句柄时，成功交付值元组，Sender 的停止终态交付空值，错误由等待方接收。[Sender 桥接示例](../../examples/experimental/execution/sender_bridge.cpp)用 `stdexec::then` 计算总价，再异步领取结果。该目标链接 `faio::experimental_execution` 并使用锁定的 stdexec。构建与运行命令见 [实验示例索引](../../examples/README.md#实验示例)。

## 任务生命周期注解

头文件 `<faio/experimental/lifecycle.h>` 与目标 `faio::experimental_lifecycle` 提供 `[[=faio::experimental::lifecycle<Policy>{}]]`。真实协程在开始执行及退出时调用策略的 `before()`、`after(const task_exit&) noexcept`；普通任务转发函数不应用此注解。

此组件独立启用，要求同一程序中涉及任务的翻译单元统一使用 C++26 与 `FAIO_EXPERIMENTAL_TASK_LIFECYCLE=1`。适配会增加一个辅助协程帧。[生命周期示例](../../examples/experimental/lifecycle/annotated_task.cpp)展示任务创建、成功与失败三条路径。退出状态和异常传播约定见 [协程与并发](tasks.md#任务生命周期策略)。
