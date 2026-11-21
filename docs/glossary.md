# 术语表

这里按字母顺序解释 faio 文档里反复出现的概念。术语之间彼此关联，第一次读可以挑感兴趣的看，遇到不懂的词再回来查。

## Async / 异步

异步代码指用 `co_await` 编写的代码，它让很多任务能在少数几个线程上并发执行。异步不等于并行：任务可能在同一个线程上交错推进，也可能在不同线程上同时运行。

## Awaiter / 可等待对象

能写在 `co_await` 后面的对象叫可等待对象（awaitable）。它实现三个方法：`await_ready()` 检查是否已就绪，`await_suspend(handle)` 在需要等待时登记协程，`await_resume()` 在恢复时交付结果。

faio 的 `task`、I/O 操作、定时器和同步原语等待都是可等待对象。`awaiter_facade` 提供两阶段的 `on_suspend` / `on_resume` 协议，用于自定义操作。

## 并发与并行

并发（concurrency）和并行（parallelism）相关但不同。并行一定并发，并发不一定并行。两个任务交替执行、从不同时运行，是并发而非并行。faio 的运行时既支持多个 worker 上的并行，也支持单 worker 上的并发。

## Coroutine / 协程

协程是可以挂起并在之后恢复的函数。编译器把局部变量和挂起位置保存在协程帧里。faio 的协程函数返回 `faio::task<T>`：`co_await` 处挂起时，线程转去做别的事；结果就绪后从挂起点继续执行。

## Executor / Scheduler / 执行器与调度器

执行器负责推进任务，调度器决定任务在哪个线程上、什么时候运行。faio 的调度器在编译期固定为 `default_scheduler`（即 `domain_scheduler`），任务不经动态虚表。调度器引用通过线程本地的 `execution_thread_binding` 传递。

## expected

`faio::expected<T>` 是 `std::expected<T, faio::Error>` 的别名。faio 的 I/O 操作用它把失败作为值返回，调用方必须先检查成功状态再解引用。它不会自动变成异常。

## Future

Future 是表示一个尚未完成操作的值，C++ 里对应 `promise`/`future` 这套机制。faio 主要用协程的 `task<T>` 表达异步计算——它比手写 future 链更接近顺序代码，不过底层仍依赖同样的挂起与恢复机制。Rust 语境里的 Future 对应 C++ 的 coroutine，faio 文档里提到"future"时多指这个概念。

## I/O 域（io_domain）

`io_domain` 是 faio 完成端的统一抽象。它拥有后端包装、稳定的操作槽池和完成消费者，负责仲裁完成、取消、关闭与停机，并发布终态。同一时刻只有一个线程消费完成事件。

## join_handle

`spawn()` 返回的结果句柄。通过 `co_await` 它领取任务结果，通过 `request_stop()` 提出协作停止请求，通过 `done()` 非阻塞地查询状态。销毁句柄不会取消任务。

## 阻塞池

独立于协程 worker 的线程池，用来执行 `spawn_blocking()` 提交的同步函数，以及 Reactor 后端上的普通文件操作。阻塞池里的代码可以调用同步 API，不会卡住协程 worker。

## Proactor / Reactor

两种 I/O 完成模型。Proactor 由操作系统完成整个操作后再通知程序（如 io_uring、IOCP）；Reactor 只通知"就绪"，程序还要自己发起 syscall（如 epoll、kqueue）。faio 把 Reactor 提升成和原生 Proactor 相同的完成语义，上层看到的是同一套接口。

## Runtime / 运行时

运行时包含调度器，以及与之配套的定时器、I/O、线程池等设施。faio 提供两种模式：`multi_thread` 用多个 worker 驱动任务，支持工作窃取；`current_thread` 由调用 `block_on()` 的线程驱动，不创建额外 worker。

## stop_token / 停止令牌

`std::stop_token` 让任务观察协作式停止请求。faio 里通过 `co_await this_coro::stop_token()` 取得，`join_handle::request_stop()` 或任务组的 `request_stop()` 发出请求。任务自己决定何时退出，不会被强制销毁。

## task

`faio::task<T>` 是 faio 最基本的并发与调度单元。它是惰性、移动独占、一次性的：调用协程函数只创建任务，`co_await` 或 `spawn()` 才启动。不可复制，也不可重复等待。

## 协作取消

一种停止方式：发出请求的一方不能强制终止任务，任务通过检查停止令牌自行决定何时退出。这样任务有机会做清理。与"强杀线程"相对。

## 工作窃取

一种负载均衡策略：空闲的 worker 从其他 worker 的队列里取任务来执行。faio 的多线程运行时默认支持；任务可以用 `.with_no_stealing()` 收紧这一行为。

## 结构化并发

一种组织任务的方式：任务在词法作用域内创建，作用域返回前必须等它们全部退出。`scope`、`task_group`、`with_stream`、`complete_before` 都遵循这一原则，避免留下悬空的工作。

---

[专题首页](topics/index.md) · [教程](learn-faio.md)
