# 认识 faio

faio 是一个基于 C++23 协程的异步 I/O 头文件库。异步代码按顺序书写：等待 I/O 时协程挂起，线程继续执行其他任务，操作完成后再从挂起点恢复。

本教程以仓库中的两个示例为主线：

- [`examples/quickstart.cpp`](../examples/quickstart.cpp)：最小异步程序，用来介绍任务、`co_await`、`join` 和 `block_on`。
- [`examples/net/tcp_echo.cpp`](../examples/net/tcp_echo.cpp)：TCP 回显服务，用来介绍监听、接受连接、为每条连接派生任务，以及读写和错误处理。

两章分别对应一个示例。正文只解释关键部分，完整代码以仓库文件为准。

## 阅读顺序

1. 先读 [Hello faio](tutorial/hello-faio.md)，从 quickstart 建立最小模型。
2. 再读 [TCP 回显服务器](tutorial/tcp-echo-server.md)，看 tcp_echo 如何把任务模型用于连接处理。

构建和运行方式见[快速开始](getting-started.md)。

[下一章：Hello faio →](tutorial/hello-faio.md)
