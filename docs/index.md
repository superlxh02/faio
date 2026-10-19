# 欢迎使用 faio

**现代 C++ 的异步答案。**

faio（Fast Async I/O）是完全基于 C++20 协程的现代 C++ 异步基础设施：header-only，采用 Proactor 完成模型，可以理解为 C++ 版本的 tokio。运行时、网络、文件、定时器与协程同步原语都在同一套 API 下，跨 Linux / macOS / Windows。基础库要求 **C++23**（需要协程、`std::expected` 与 `std::format`），采用 Apache 2.0 许可证。

等待 I/O、定时器或一把协程锁时，协程挂起并把 worker 还给运行时；完成事件到达后，从 `co_await` 的下一行继续。没有回调，也不需要额外的线程替它等待。

## 从这里开始

| 你想做什么 | 阅读入口 |
| --- | --- |
| 编译并运行第一个程序 | [快速开始](getting-started.md) |
| 选择平台、编译器和 I/O 后端 | [平台与构建](platforms.md) |
| 理解惰性任务、所有权、取消与错误 | [核心概念](concepts.md) |
| 查找具体类型、方法和返回值 | [API 参考](api/index.md) |
| 学习可以直接运行的程序 | [官方例程](examples/index.md) |
| 理解内部调度与 I/O 协议 | [模块讲解](modules.md) |

## 一个小程序

这份源码同时构建为 `quickstart` 可执行程序，首页也从同一文件生成代码展示。

```cpp
{{#include ../examples/quickstart.cpp}}
```

运行输出：

```text
The answer is 42
```

`delayed_value()` 创建惰性任务。`spawn()` 立即提交，返回可等待的结果句柄；`join()` 在被等待时并发启动两个分支，按传入顺序返回结果；`block_on()` 是普通线程进入异步程序的同步入口。

## 三个核心概念

- **`faio::task<T>`**：一个返回 `task` 的函数就是一个**协程**。它是**惰性**的——调用只构造任务，`co_await`、`block_on` 或 `spawn` 之后才启动；也是**异步**的——等待时挂起自己，把 worker 交还运行时。
- **并发接口**：`spawn` 提交并拿到结果句柄，`join` 并发跑完按序取值，`select` 竞速并取消其余分支，`scope` 收拢动态派生的子任务。
- **`faio::expected<T>`**：网络与文件操作的结果类型，即 `std::expected<T, faio::Error>`，带错误域、原生错误码与已传输字节数。

详细说明见[核心概念](concepts.md)。

## 平台与能力

| 平台 | I/O 后端 | 说明 |
| --- | --- | --- |
| Linux | io_uring 或 epoll | 编译时选择，使用各自独立构建目录 |
| macOS | kqueue | 使用满足 C++23 要求的 Homebrew LLVM |
| Windows | IOCP / OVERLAPPED | MSVC、clang-cl 或满足项目要求的 MinGW-w64 |

普通文件操作在具备原生异步支持时使用原生路径，其余操作由独立、有界的文件服务处理。用户提交的阻塞函数使用另一组阻塞线程，避免占用协程 worker。

## 文档如何组织

- **用它**：[快速开始](getting-started.md) 构建第一个程序；[核心概念](concepts.md) 说明惰性任务、缓冲区生命周期、两类错误与协作取消这些跨模块约定；[API 参考](api/index.md) 查具体签名、参数和返回值。
- **看它怎么跑**：[官方例程](examples/index.md) 直接包含仓库里的 `.cpp`，页面上的代码就是编译运行的那一份。
- **读它怎么实现**：[模块讲解](modules.md) 按协程封装、并发、同步、运行时、I/O、网络与定时器分章解析源码。

项目源码：[GitHub 仓库](https://github.com/superlxh02/faio)。问题与改进建议可通过 [Issues](https://github.com/superlxh02/faio/issues) 提交。
