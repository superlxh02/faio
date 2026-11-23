# Hello faio

本章实现第一个异步程序：等待 10 毫秒后输出 `Hello, world!`。程序覆盖四个基础概念：`task`、`co_await`、`join` 和 `block_on`。完整代码见仓库中的 [`examples/quickstart.cpp`](../../examples/quickstart.cpp)。

## 程序结构

程序由三个函数组成：

```cpp
faio::task<void> hello() {
  co_await faio::time::sleep(10ms);
  std::print("Hello, world!\n");
}

faio::task<void> run() {
  co_await faio::join(hello());
}

int main() {
  faio::block_on(run());
}
```

构建和运行方式见[快速开始](../getting-started.md)。运行后输出：

```text
Hello, world!
```

## `task<void>` 是惰性的

`hello()` 是协程函数，返回 `faio::task<void>`。调用它只会创建协程帧，不会执行函数体。任务需要被 `co_await` 等待，或者交给 `join()` 组合后再等待；同步入口则由 `block_on()` 驱动。

`task` 还是移动独占的，不能复制，一个协程帧只能有一个所有者。`task<void>` 表示任务只报告完成，不携带返回值。

## `co_await` 挂起协程

`hello()` 中的关键操作是：

```cpp
co_await faio::time::sleep(10ms);
```

同步的 `sleep` 会让线程停住 10 毫秒。`co_await` 不阻塞线程：它挂起当前协程，把线程交还给运行时，运行时可以继续执行其他任务；定时器到期后，协程从下一行恢复。

## `join()` 组合任务

`run()` 用 `join()` 把 `hello()` 纳入一个组合任务：

```cpp
co_await faio::join(hello());
```

`join()` 把传入的任务组合成一个新任务。在 quickstart 中，它等待 `hello()` 完成，并把异常原样传播出来。`join()` 本身也是惰性的，真正驱动执行的是外层的 `co_await`。

## `block_on()` 驱动任务

`main()` 是普通函数，不能写 `co_await`，因此用 `block_on()` 驱动：

```cpp
faio::block_on(run());
```

`block_on()` 在当前线程上运行任务，直到任务完成。任务体抛出异常时，异常会在这里按原类型重新抛出。

只有同步入口才需要 `block_on()`。

## 小结

这个程序展示了 faio 的最小使用方式：用 `task` 表示异步工作，用 `co_await` 等待，用 `join()` 组合，用 `block_on()` 启动。下一章把这套模型用于 TCP 回显服务：监听端口，为每条连接派生任务，持续读写。

[教程首页](../learn-faio.md) · [下一章：TCP 回显服务器 →](tcp-echo-server.md)
