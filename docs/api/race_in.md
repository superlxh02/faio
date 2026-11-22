# race_in

任务已经交给一个任务组，想先拿走最先完成的那个结果，让落选分支在组退出时慢慢收尾时，用 `race_in`。它不等落选分支就返回，排空工作留给任务组。先包含 `<faio/coroutine/composition.hpp>`。

```cpp
template <class S, class... Ts>
  requires(sizeof...(Ts) != 0)
task<race_result<Ts...>, S> race_in(basic_task_group_context<S>& group, task<Ts, S>... children);
```

调用位置必须在任务组主体的词法范围内，传入该组的 `basic_task_group_context<S>&`，并且要在主体返回前启动竞速。分支借用调用方的数据时，这些数据必须一直有效到整个任务组退出，只活到 `race_in` 返回并不够。

等待时并行启动所有分支，任意一个分支先到达 value、error 或 stopped 终态，就会成为胜出者，组合随后向其余分支发出协作停止请求并立即返回。`operation_cancelled` 按 `settle` 的规则归类，因此一个正常停止的分支也可能先胜出，`outcome` 里对应 `stopped` 终态。

`race_result<Ts...>` 的字段：

| 字段 | 含义 |
| --- | --- |
| `index` | 胜出分支在原始参数中的位置 |
| `outcome` | 同形状的 `std::variant<task_outcome<Ts>...>`，按 `index` 对应位置读取 |
| `loser_error(i)` | 落选分支退出时发布的异常，尚未发布时为空 |

落选分支的成功值和停止原因都不会出现在返回值里。`loser_error(i)` 只报告已经发布的异常，分支还没有退出时可能为空；想让诊断确定，先等整个任务组退出。提交某个分支失败时，`race_in` 会请求停止并抛出原异常，组在主体退出时排空已经接纳的分支。

代价是一个共享的竞速状态，里面为每个分支保留一个结果槽。胜出后 `race_in` 不等落选分支，但它们在组退出前仍占用协程帧和调度配额。

完整程序见 [进阶竞速](../../examples/coroutine/race_strategies.cpp)。
