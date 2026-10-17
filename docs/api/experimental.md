# 实验性接口

实验层要求 Linux + GCC 16.1.x + libstdc++ 16 + C++26，通过 `FAIO_ENABLE_EXPERIMENTAL` 独立启用。基础库用户可以保持此开关关闭。

## 公共头文件

| 头文件 | 用途 |
| --- | --- |
| `<faio/experimental/async_main.h>` | 通过注解声明异步程序入口 |
| `<faio/experimental/runtime_options.h>` | 声明运行时模式、worker 和后端配置 |
| `<faio/experimental/runtime.h>` | 获取当前运行时以及显式运行时引用 |
| `<faio/experimental/execution.h>` | 与 stdexec Sender/Receiver 互操作 |

## 异步入口

`faio::experimental::main` 注解指定返回 `faio::task<int>` 的入口，`runtime_options` 注解设置运行时配置。入口的整数结果用作程序退出状态。

完整例程直接引用仓库源码：

```cpp
{{#include ../../examples/experimental/async_main.cpp}}
```

## Sender/Receiver 互操作

通过 `faio::experimental::this_runtime()` 取得当前运行时引用，再选择对应操作：

| 调用 | 用途 |
| --- | --- |
| `runtime.get_multi_thread_scheduler()` | 获取可用于 stdexec 调度的多线程 scheduler |
| `runtime.as_sender(task)` | 将 faio 任务接入 sender 图 |
| `runtime.spawn_sender(sender)` | 提交 sender 图，返回可等待的结果句柄 |

这些接口不更改基础库的任务和运行时使用方式。构建与完整调用组合见 [实验性例程](../examples/experimental.md)。
