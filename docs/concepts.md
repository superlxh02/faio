# 核心概念

这些约定贯穿所有 API，先理解它们，再选择具体模块。

## 任务是惰性的

调用返回 `task<T>` 的协程函数只创建任务。直接 `co_await`、传给 `block_on()`，或者通过 `spawn()` 提交之后，任务才开始执行。

```cpp
auto pending = delayed_value(42);              // 尚未开始
auto running = faio::spawn(std::move(pending)); // 提交并发任务
const int value = co_await running;            // 挂起当前协程，等待结果
```

上面是协程内部的用法片段；`delayed_value()` 的完整定义见 [快速开始](getting-started.md)。任务和结果句柄都应按移动所有权管理；句柄的结果只能领取一次。

## 同步入口与异步等待

普通线程使用 `block_on()` 进入运行时；协程内部用 `co_await`。不要在 worker 内调用 `block_on()`、同步句柄 `get()` 或阻塞等待自己的任务。

`join()` 等待所有分支，`select()` 在选出完成分支后请求其余分支取消并等待退出，因此竞速返回也可能需要等待取消排空。`scope` 适合管理动态派生的子任务。

## 对象与缓冲区的生命周期

异步操作可能持有调用方的缓冲区引用，直到操作最终完成。挂起期间不能让缓冲区析构、重分配或被其他任务无序修改。资源的关闭请求也不意味着所有在途操作已经完成。

协程 lambda 特别要注意捕获闭包的生命周期。官方例程优先采用无捕获 lambda，通过参数把值放入协程帧；引用参数仍须由调用方确保存活。

同一 stream 支持一个读方向和一个写方向操作同时进行；同方向冲突返回 `EBUSY`。空缓冲区读取成功不表示 EOF；只有非空读取返回零字节才能作为对端结束信号。

## 错误分两类处理

- 网络与文件操作通常返回 `faio::expected<T>`，它是 `std::expected<T, faio::Error>` 的别名。检查结果再解引用，用 `error()` 读取失败原因；`Error` 保留错误域、原生错误码与已经传输的字节数。
- 协程任务中的异常会沿等待链传播。时间等待和协作取消可能通过异常报告，具体遵循对应接口说明。

```cpp
const auto count = co_await stream.read(buffer);
if (!count) {
  // 检查 count.error()，决定记录、重试或结束连接。
  co_return;
}
if (*count == 0) co_return; // buffer 非空时表示 EOF
```

此处为连接处理函数内的片段；完整错误处理见 [TCP 官方例程](examples/network.md)。不要对未检查的 `expected` 直接使用 `*result` 或 `result->member`。

## 取消是协作协议

运行时通过停止请求通知任务，能够响应的等待点完成取消；长计算和任意同步第三方函数不会自动中断。需要耗时同步调用时，使用 `spawn_blocking()`；它把函数提交到独立的有界线程池，等待结果只挂起协程。

`spawn_detached()` 不返回结果句柄。任务内部应处理异常，并确保它引用的数据活到任务结束。

## 关闭是生命周期的一部分

在首次使用前调用 `runtime::configure()`。默认运行时首次使用时启动，`runtime::shutdown()` 排空后关闭，之后不能重新配置或重新启动。

单线程模式由调用 `block_on()` 的线程驱动；未驱动时，排队任务和异步事件不会自行向前推进。详细规则见 [运行时 API](api/runtime.md) 和 [运行时设计](异步运行时.md)。
