# join_all_settled

任务数量在运行时才确定、类型一致，并且需要保留每一项的结局时，用 `join_all_settled`。收集规则与 `join_settled` 相同，输入换成 `std::vector`，结果也是同长度的 vector。先包含 `<faio/coroutine/composition.hpp>`。

```cpp
template <class T, class S>
task<std::vector<task_outcome<T>>, S> join_all_settled(std::vector<task<T, S>> children);
```

等待时并行启动 vector 中的全部任务，等它们全部退出后按输入下标返回 `std::vector<task_outcome<T>>`。返回长度与输入相同，失败或停止的项保留原下标。每个槽独立保存 value、error 或 stopped；组合使用 `collect_all` 策略，某一项失败不会让其他任务收到停止请求。

```cpp
auto outcomes = co_await faio::join_all_settled(std::move(queries));
for (auto& outcome : outcomes) {
  if (outcome.status() == faio::task_outcome_status::value)
    prices.push_back(outcome.take_value());
}
```

输入为空时直接交付空 vector，不创建任务组。父任务请求停止后，已接纳的子任务会收到协作停止，因停止而退出的项对应槽记为 `stopped`；组合仍等它们退出后才返回。`operation_cancelled` 的归类与 `settle` 一致：子帧记录且停止令牌收到过请求才算 `stopped`，否则算 `error`。

启动子任务或预留组合恢复槽失败属于基础设施失败，组合会先排空已接纳的任务，再把异常抛给等待方。子任务自己的异常留在对应结果槽，不会从 `join_all_settled` 直接抛出。`task_outcome` 只能移动，成功值只能领取一次。

需要定位失败时，遍历结果并用下标 `i` 对应原输入中的第 `i` 个任务；`exception()` 给出异常指针，`stop_reason()` 只在停止时有意义。

所有任务会同时接纳，输入较长时协程帧、结果槽和调度压力都随数量线性增长。需要并发窗口时改用 `join_all_limited`。可运行的批处理程序见 [批处理与任务终态](../../examples/coroutine/batch_results.cpp)，其中逐项结算、固定批次和限并发批次共用同一套 `task_outcome` 处理方式；把固定参数换成 `std::vector` 就是本页接口的用法。
