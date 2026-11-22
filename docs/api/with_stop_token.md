# with_stop_token

需要让一项局部工作同时响应父任务和外部 `std::stop_token` 时用它。外部令牌通常来自应用层取消、信号处理或另一项任务的退出通知。先包含 `<faio/coroutine/lifecycle.hpp>`。

```cpp
template <class T, class S>
task<T, S> with_stop_token(std::stop_token token, task<T, S> work);
```

调用时把工作和要附加的令牌一起传入，返回的任务仍然惰性执行。父任务已有的停止连接保持不变，传入令牌作为额外来源接入；任一来源请求停止，子任务及其继承该上下文的后代都会看到停止请求。

停止是协作式的。`with_stop_token` 只负责转发请求，不会中断正在运行的代码；子任务需要在等待点或检查点响应，最终以 `operation_cancelled` 或其他异常结束。正常值、异常和取消都沿用 `task<T, S>` 的原生交付规则。

如果传入的令牌在调用前已经请求停止，子任务启动时就能观察到该状态。传入空令牌时不会建立额外连接，行为与直接等待 `work` 相同。被包装任务的停止连接随返回的任务帧存在，任务结束或销毁后自动解除。

```cpp
std::stop_source external;
auto pending = faio::spawn(
    faio::with_stop_token(external.get_token(), load_profile()));
external.request_stop();
try {
  co_await pending;
} catch (const faio::operation_cancelled&) {
  // 工作已响应停止并退出
}
```

需要明确区分成功、失败和取消时，可以在局部边界内配合 `settle` 使用。

代价是一层额外的组合协程帧和一个停止连接，子任务的执行路径不变。它适合短生命周期的局部包装，不适合替代任务组或作用域提供的结构化停止。

完整程序见 [停止与截止](../../examples/coroutine/cancellation_deadline.cpp)。
