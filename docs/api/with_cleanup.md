# with_cleanup

任务体结束时需要运行一段可能挂起的清理逻辑，例如关闭句柄、回滚部分写入或释放借出的缓冲，可以用这个组合。清理使用单独的停止上下文，不会因为任务体收到停止请求而直接中断。先包含 `<faio/coroutine/lifecycle.hpp>`。

```cpp
template <class Body, class Cleanup, class Work = std::invoke_result_t<Body&>>
  requires std::same_as<Work, task<typename Work::value_type, typename Work::scheduler_type>>
        && std::same_as<std::invoke_result_t<Cleanup&, const task_exit&>,
                        task<void, typename Work::scheduler_type>>
Work with_cleanup(Body body, Cleanup cleanup);
```

`with_cleanup` 是惰性组合：等待返回的任务时先调用 `body` 取得工作任务，等它退出后再调用 `cleanup`。`cleanup` 收到一个 `task_exit`，其中 `status` 为 `succeeded`、`failed` 或 `cancelled`，`exception` 保存对应异常。清理工厂必须返回与任务体相同静态调度器的 `task<void, S>`，可以挂起并等待其他异步操作。清理恰好执行一次。

任务体与清理都失败时，`with_cleanup` 抛出 `cleanup_error`。`primary()` 返回任务体一侧的异常，`secondary()` 返回清理一侧的异常。只有一侧失败时直接传播该侧原异常。成功值在清理完成后才移动给等待方，因此等待方看到的资源已经完成收束。

任务体工厂和清理工厂都按值移动进组合帧，调用方不需要让它们比返回的任务活得更久。清理工厂在任务体退出后才被调用，可以捕获任务体使用的局部状态；这些状态的寿命必须覆盖清理的整个等待过程。

`task_exit` 记录只在清理任务存活期间有效。清理逻辑如果把该记录交给后代任务，需要先等这些后代退出再返回。

同步的任务体工厂抛出 `operation_cancelled` 时，只有在主体有效令牌已经收到停止请求的情况下才记为取消，否则按失败处理。

如果任务体工厂在启动前就抛出异常，清理仍会执行，并收到失败记录。

每次调用增加一层组合协程帧和一个独立停止状态。清理与任务体串行执行，总耗时包含清理时间。

完整程序见 [协程生命周期示例](../../examples/coroutine/lifecycle.cpp)。
