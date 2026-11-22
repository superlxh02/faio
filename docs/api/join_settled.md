# join_settled

固定数量的异构任务需要全部跑完，并且每个结果都要单独保留下来时，用 `join_settled`。某一项失败不会让其他任务提前结束，返回的 tuple 与参数一一对应。先包含 `<faio/coroutine/composition.hpp>`。

```cpp
template <class S = default_scheduler, class... T>
task<std::tuple<task_outcome<T>...>, S> join_settled(task<T, S>... children);
```

等待这个任务时，会先启动全部子任务，再按参数顺序逐项等待；全部退出后按参数顺序返回 `std::tuple<task_outcome<T>...>`。每个槽独立保存成功值、异常或停止，`void` 子任务同样保留位置，只是没有可领取的值。

每个槽的 `task_outcome<T>` 有三种终态：

| `status()` | 内容 | 读取方式 |
| --- | --- | --- |
| `value` | 成功值 | `take_value()`，只能领取一次 |
| `error` | 原异常 | `exception()`，或用 `value_or_throw()` 重抛 |
| `stopped` | 停止原因 | `stop_reason()`，可能没有原异常 |

组合内部使用 `collect_all` 策略，子任务失败不会触发对兄弟任务的停止请求，因此结果里可能同时出现 value 和 error。父任务请求停止后，组内子任务会收到协作停止请求，因停止而退出的项对应槽记为 `stopped`。子任务抛出的 `operation_cancelled` 沿用 `settle` 的判定：只有子帧记录且停止令牌确实收到请求才算 `stopped`，否则算 `error`。

子任务自己的异常留在结果槽里，不会从 `join_settled` 直接抛出。启动子任务或预留组合恢复槽失败属于基础设施失败，会先排空已接纳的子任务，再把原异常抛给等待方。参数包为空时返回空 tuple。

`task_outcome` 只能移动，成功值只能领取一次。每个子任务各占一个协程帧和结果槽，返回 tuple 的元素个数与参数个数相同。数量在运行时决定时用 `join_all_settled`，需要限制同时运行的数量时用 `join_all_limited`。

完整程序见 [批处理与任务终态](../../examples/coroutine/batch_results.cpp)。
