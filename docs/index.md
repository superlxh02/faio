# 欢迎使用 faio

**用现代 C++，写清晰的异步程序。**

faio（Fast Async I/O）是以头文件形式提供的跨平台异步 I/O 库。它基于 C++ 协程，提供任务与并发组合、单 / 多线程运行时、网络与文件 I/O、定时器和协程同步原语。基础库要求 **C++23**，采用 Apache 2.0 许可证。

异步操作等待时挂起协程，把工作线程交还给运行时；完成后，代码从 `co_await` 的下一行继续执行。

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

`delayed_value()` 创建惰性任务。`join()` 在被等待时并发启动两个分支，按传入顺序返回结果；`block_on()` 是普通线程进入异步程序的同步入口。

## 平台与能力

| 平台 | I/O 后端 | 说明 |
| --- | --- | --- |
| Linux | io_uring 或 epoll | 编译时选择，使用各自独立构建目录 |
| macOS | kqueue | 使用满足 C++23 要求的 Homebrew LLVM |
| Windows | IOCP / OVERLAPPED | MSVC、clang-cl 或满足项目要求的 MinGW-w64 |

普通文件操作在具备原生异步支持时使用原生路径，其余操作由独立、有界的文件服务处理。用户提交的阻塞函数使用另一组阻塞线程，避免占用协程 worker。

## 文档如何组织

API 参考按公开命名空间介绍调用方式、参数、返回值和错误处理；例程页直接包含仓库 `.cpp`，避免维护两份源码。模块设计部分保留 `docs/` 中原有的深度讲解，并为架构图增加本地 Mermaid 渲染。

项目源码：[GitHub 仓库](https://github.com/superlxh02/faio)。问题与改进建议可通过 [Issues](https://github.com/superlxh02/faio/issues) 提交。
