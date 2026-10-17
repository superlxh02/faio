# 实验性例程

实验层探索 C++26 反射注解的运行时配置、异步入口，以及 stdexec Sender/Receiver 互操作。它由独立开关控制，基础库不依赖这些实验接口。

## 环境与构建

要求 Linux + GCC 16.1.x + libstdc++ 16 + C++26，并预先准备实验依赖。普通跨平台工具链不能直接构建这一组例程。

```sh
cmake --preset linux-gcc16.1-experimental-dual
cmake --build --preset linux-gcc16.1-experimental-dual \
  --target async_main execution_tcp_server -j4
```

预设名称中的 dual 沿用仓库命名；基础 Linux I/O 后端仍遵循构建时固定选择。依赖与平台门禁以 [实验性模块讲解](../实验性特性.md) 为准。

## 选择例程

| 源码 | 展示内容 |
| --- | --- |
| [async_main.cpp](../../examples/experimental/async_main.cpp) | 注解声明入口与运行时配置 |
| [async_main_execution.cpp](../../examples/experimental/async_main_execution.cpp) | 注解入口中提交 sender 图 |
| [execution_current_thread.cpp](../../examples/experimental/execution_current_thread.cpp) | 单线程模式下的 task / sender 桥接 |
| [execution_multi_thread.cpp](../../examples/experimental/execution_multi_thread.cpp) | 在 faio scheduler 上调度 sender 图 |
| [execution_tcp_server.cpp](../../examples/experimental/execution_tcp_server.cpp) | sender 图驱动 TCP 服务 |

## 完整源码：注解入口

```cpp
{{#include ../../examples/experimental/async_main.cpp}}
```

## 完整源码：Sender 互操作

```cpp
{{#include ../../examples/experimental/async_main_execution.cpp}}
```

反射语法的自动索引可能受 Doxygen 版本限制。完整协议、所有权、取消传播与入口生成逻辑见 [实验性特性](../实验性特性.md)。
