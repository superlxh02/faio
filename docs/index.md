# faio 文档

faio 是基于 C++ 协程的跨平台异步 I/O 头文件库，提供任务、运行时、网络、文件、定时器和同步原语。主线使用 C++23，TLS 与异步进程通过独立开关启用。除 CMake 外，Bazel 与 xmake 也能构建这个库，并提供 C++20 具名模块（`import faio;`）。

| 入口 | 内容 |
| --- | --- |
| [快速开始](getting-started.md) | 工具链、依赖、CMake/Bazel/xmake 构建、具名模块与工程集成 |
| [认识 faio · 教程](learn-faio.md) | 以一个 TCP 回显服务贯穿十章，从第一个异步程序走到异步流 |
| [专题](topics/index.md) | 同步桥接、优雅停机、错误处理、日志与单元测试 |
| [术语表](glossary.md) | 协程、任务、运行时、协作取消等核心概念的解释 |
| [API 介绍](api/index.md) | 公共头文件、调用方式与生命周期约定 |
| [API 查询](api-reference.md) | 类型、函数、重载、参数和返回值检索 |

源码与问题反馈：[GitHub](https://github.com/superlxh02/faio)。
