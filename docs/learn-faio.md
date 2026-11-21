# 认识 faio

这个教程会一步一步带你用 faio 写出一个异步 TCP 回显服务。从最简单的异步程序开始，逐步加入并发、共享状态、通道、网络 I/O，最后处理取消和超时。读完一遍，faio 的核心用法你就都摸过一遍了。

faio 是一个基于 C++ 协程的异步 I/O 头文件库。写出来的代码看起来是顺序执行的，等待 I/O 时却不会占住线程——协程挂起，线程转去做别的事，等数据就绪了再回来接着跑。它同时提供 tokio 风格的并发组合、同步原语和跨平台 I/O。

## 我们将要构建什么

贯穿整个教程的是一个 TCP 回显（echo）服务：客户端发过来什么，服务端就原样发回去。它足够简单，能让我们把注意力放在异步本身；又足够完整，能覆盖 faio 的大部分核心接口。

每章都在这个服务上加一点东西：

| 章节 | 这一章加上什么 | 用到的核心接口 |
| --- | --- | --- |
| [环境准备](tutorial/setup.md) | 编译器、依赖和一个能跑起来的工程 | CMake、`faio::faio` |
| [Hello faio](tutorial/hello-faio.md) | 第一个异步程序 | `task`、`block_on`、`sleep` |
| [生成任务](tutorial/spawning.md) | 让多个连接并发处理 | `spawn`、`join_handle`、`join` |
| [共享状态](tutorial/shared-state.md) | 在连接之间共享一份配置 | `mutex`、`condition_variable`、`semaphore` |
| [通道](tutorial/channels.md) | 把处理结果汇集到一处 | `mpsc`、`latch`、`barrier` |
| [网络 I/O](tutorial/io.md) | 真正地监听端口、读写字节 | `TcpListener`、`TcpStream` |
| [分帧](tutorial/framing.md) | 把字节流切成一条条消息 | `BufReader`、`BufWriter` |
| [深入 task](tutorial/task-in-depth.md) | 搞懂任务是怎么挂起和恢复的 | `awaiter`、`this_coro` |
| [select 与并发组合](tutorial/select.md) | 竞速、截止与异常边界 | `select`、`race_in`、`complete_before` |
| [异步流](tutorial/streams.md) | 按序列处理源源不断的数据 | `with_stream`、`for_each_concurrent` |

## 读这个教程之前

假设你已经会用 C++ 协程写 `co_await`，也熟悉现代 CMake。没写过协程也没关系，[Hello faio](tutorial/hello-faio.md) 那一章会从最小的一段代码讲起。你也不需要事先了解 tokio，教程里用到 tokio 概念的地方都会顺便解释。

faio 的主线要求 C++23。编译器要支持协程、`std::expected`、`std::format` 和显式对象参数，CMake 会在配置阶段逐一检查。具体的版本要求和三个平台的依赖清单见[环境准备](tutorial/setup.md)。

## 怎么读

建议按顺序读。每一章都建立在前一章的代码上，节奏也一样：先说这章要达到什么目标，再给出完整代码，然后逐段拆解，最后让你运行一遍看看效果。

不想按顺序读也可以。每章都能独立看懂，代码也都可以单独编译运行。想了解某个具体场景的做法，去看[专题](topics/index.md)；想查某个模块的参数和返回值，用 [API 介绍](api/index.md)或 [API 查询](api-reference.md)。

如果在教程里卡住了，欢迎到 [GitHub Discussions](https://github.com/superlxh02/faio/discussions) 或 [Issues](https://github.com/superlxh02/faio/issues) 提问。问“基础”问题完全没问题，大家都是从这一步开始的。

---

[下一章：环境准备 →](tutorial/setup.md)
