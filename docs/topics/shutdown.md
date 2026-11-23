# 优雅停机

程序退出时，数据库连接可能还在写，客户端请求可能处理到一半，直接停线程会留下未完成的写入。优雅停机需要决定何时停、怎样通知各部分停、以及怎样等它们真正停完。

## 停机分三步

不管用什么语言，优雅停机都绕不开这三件事：

1. **决定何时停机**：收到退出信号、达到某个条件，或者用户主动取消；
2. **通知所有相关部分**：让每个任务都知道该收尾了；
3. **等待它们退出**：确认收尾完成，再释放资源和关闭运行时。

faio 为每一步都提供了工具。这一步步来。

## 第一步：决定何时停机

最常见的触发条件是操作系统信号，比如终端里的 Ctrl+C。faio 本身不提供信号监听，你可以用同步的 `std::signal`，或者接入自己项目的信号处理，然后把信号转成一个停止请求。

多个停机条件也可以合并。比如"收到 Ctrl+C"或"客户端主动断开"任一发生就停：

```cpp
faio::task<void> wait_for_shutdown(faio::sync::mpsc<void>& signals) {
  // 把各种停机来源汇集到同一个通道
  co_await signals.recv();
}
```

也可以用 `select()` 同时等几个来源，谁先到就算谁。这一点在[协程与并发](../api/tasks.md)那一章讲过。

很多应用并不需要信号监听。测试里就是一个显式的停止请求，服务内部出错了也可以主动触发。关键是让"停机"变成一个能被任务观察到的信号。

## 第二步：通知各部分

faio 的取消是**协作式**的：`request_stop()` 只是提出请求，任务自己决定什么时候退出。这与"强杀线程"有本质区别：任务有机会关闭文件、写回状态、释放借用。

任务通过停止令牌观察请求：

```cpp
faio::task<void> worker() {
  const auto token = co_await faio::this_coro::stop_token();
  while (!token.stop_requested()) {
    // 处理一项工作
    co_await faio::this_coro::yield_if_needed();
  }
  // 在这里做收尾
}
```

`join_handle::request_stop()` 可以让单个任务停；`scope` 和 `task_group` 的上下文也有 `request_stop()`，影响一组任务。

如果任务在等待同步原语，停止会让 `wait()` 抛出 `operation_cancelled`。捕获它，然后走正常的收尾路径：

```cpp
try {
  co_await latch.wait();
} catch (const faio::operation_cancelled&) {
  co_await cleanup();
  throw;
}
```

想让清理逻辑无论成功失败都跑，用 `with_cleanup(body, cleanup)`。它把 `task_exit` 传给你，告诉你任务是成功、失败还是被取消。

## 第三步：等待退出

`request_stop()` 返回时，任务**未必已经退出**。它只是收到了请求，可能还在处理当前一项工作，或者正在执行异步清理。所以必须等。

结构化并发在这里帮了大忙。`scope` 和 `task_group` 在主体返回时会排空自己接纳的所有孩子，不用你一个个去 `co_await`：

```cpp
co_await faio::scope([&](faio::scope_context& owner) -> faio::task<void> {
  owner.spawn(worker());
  owner.spawn(worker());
  owner.request_stop();             // 通知这一组
  co_return;                         // scope 返回前会等它们都退出
});
// 到这里可以安全释放它们借用的资源
```

`task_group` 还会汇报每个孩子的终态，方便你确认是否有任务没按预期退出。

## 关闭运行时

所有任务收尾之后，才轮到关闭运行时。faio 提供两个策略：

```cpp
faio::runtime::shutdown(faio::io::shutdown_policy::drain);      // 排空
faio::runtime::shutdown(faio::io::shutdown_policy::cancel_all); // 取消全部
```

| 策略 | 行为 |
| --- | --- |
| `drain` | 等待在途操作自然完成，再关闭。需要每个常驻任务都有退出路径 |
| `cancel_all` | 取消所有在途操作，尽快关闭 |

默认运行时 `faio::runtime::shutdown()` 用 `drain`；运行时实例的 `shutdown()` 默认用 `cancel_all`，`stop()` 默认用 `drain`。调用时注意区分。

`drain` 有个前提：**在无限循环里监听的任务不会自己停**。faio 不会替你发明服务的停止条件。如果你有一个 `for(;;) { accept() }` 的循环，必须先用停止令牌让它退出来，再调 `drain`，否则会一直等下去。

另外两条限制：

- 不要在运行时 worker 上调用 `shutdown()` 或 `configure()`，会抛 `std::logic_error`；
- 默认运行时关闭后不能重新启动。要关闭，先在一个普通线程上完成所有等待。

## 一个完整的服务骨架

把这些串起来，一个可停机的服务大致长这样：

```cpp
faio::task<void> run_service(faio::sync::mpsc<void>& shutdown_signal) {
  co_await faio::scope([&](faio::scope_context& owner) -> faio::task<void> {
    owner.spawn(accept_loop());          // 内部检查 stop_token
    co_await shutdown_signal.recv();     // 等到停机信号
    owner.request_stop();                // 通知整组退出
  });                                    // scope 在这里排空
}

int main() {
  faio::runtime::current_thread_runtime runtime;
  runtime.block_on(run_service(signals));
  // 任务都退干净了，运行时可以安全销毁
}
```

关键点在顺序：先让 accept 循环观察到停止并退出，再让 scope 排空所有孩子，最后运行时才结束。每一步都等上一步真正完成。

## 运行看看

停机逻辑散落在各个测试里，没有单独的示例程序。想看真实的调用方式，可以读 `tests/test_coroutine_on.cpp` 里对运行时实例 `stop()` 的用法。

## 下一步

停机讲的是怎么让任务体面地退出。下一个问题是任务失败时怎么办：错误该怎么表达、怎么传递、什么时候该转成异常。见[错误处理](errors.md)。


[上一章：同步与异步桥接](bridging.md) · [下一章：错误处理 →](errors.md)
