# join_all_limited

动态任务列表需要保留每项终态，又不想一次把全部任务都放出去时，用 `join_all_limited` 限制同时接纳的数量。结果按输入顺序排列，长度与输入相同。先包含 `<faio/coroutine/composition.hpp>`。

```cpp
template <class T, class S>
task<std::vector<task_outcome<T>>, S> join_all_limited(
    std::size_t limit, std::vector<task<T, S>> children, group_options options = {});
```

`limit` 是同时接纳的任务数上限，必须为正数；传入零会在调用时抛出 `std::invalid_argument`。组合从输入头部开始接纳，回收窗口中最早接纳的任务后再补充下一个，任何时刻在途任务不超过 `limit` 个。全部任务结束后按原始下标返回 `std::vector<task_outcome<T>>`，失败和停止不会压缩结果。

```cpp
auto outcomes = co_await faio::join_all_limited(4, std::move(tasks));
for (const auto& outcome : outcomes) {
  if (outcome.status() == faio::task_outcome_status::stopped
      && outcome.stop_reason() == faio::task_stop_reason::not_started)
    ++skipped;
}
```

默认策略是 `group_error_policy::fail_fast`：某个已接纳任务失败后，组请求停止，不再接纳后续任务；未启动的输入项被释放，结果槽记为 `stopped`，`stop_reason()` 为 `not_started`，已接纳的任务仍会运行到退出。传入 `group_options{group_error_policy::collect_all}` 可以让失败后继续接纳，直到所有任务都运行完。

父任务请求停止时同样停止补充窗口，未接纳项按 `not_started` 交付。子任务自己的异常保存在对应结果槽；接纳过程抛出的异常记录在对应输入项的位置，并请求停止，组合排空后返回。如果连组合自身的恢复槽都无法预留，则请求停止、排空已接纳任务，再把异常抛给等待方。

`operation_cancelled` 的归类与 `settle` 一致。已运行任务中的停止记为 `requested`，从未接纳的项记为 `not_started`。

代价是最多 `limit` 个在途任务的协程帧和结果槽，加上与输入等长的结果 vector。限并发减少同时在跑的协程帧和调度压力，不会减少总工作量。

完整程序见 [批处理与任务终态](../../examples/coroutine/batch_results.cpp)。
