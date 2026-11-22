# select_success

多个候选任务产生同一种结果，但只有第一个成功的结果有用时，用 `select_success`。失败和停止的分支不参与胜出，组合要么等到成功分支出现，要么等所有分支都失败或停止。先包含 `<faio/coroutine/composition.hpp>`。

```cpp
template <class S = default_scheduler, class... Ts>
  requires(sizeof...(Ts) > 0)
task<select_success_result<Ts...>, S> select_success(task<Ts, S>... children);
```

等待时并行启动所有分支。每个分支按 `settle` 的规则得到终态，只有 value 终态参与胜出竞争；第一个发布成功结果的分支成为赢家，组合随即向其余分支发出协作停止请求。即使赢家已经确定，也要等所有已接纳分支退出后才返回，避免落选任务继续使用调用方资源。

返回值有两种形态：

| 情况 | 字段 |
| --- | --- |
| 有成功分支 | `winner_index` 是原始参数下标，`value` 内部是对应下标的 `std::variant` |
| 全部失败或停止 | `winner_index` 与 `value` 为空，`failures` 按输入顺序记录每个分支 |

```cpp
int price = 0;  // 两个候选都返回 int
auto result = co_await faio::select_success(primary(), fallback());
if (result.winner_index)
  price = std::visit([](auto& outcome) { return outcome.take_value(); }, *result.value);
```

`failures` 中的 `branch_failure` 包含原始下标、`error` 或 `stopped` 状态、异常指针，以及停止时的可选 `stop_reason` 字段。`void` 分支的成功值在 variant 中对应 `std::monostate`。

父任务请求停止时，停止请求会传到所有分支，组合不会因此立即返回；分支逐个退出后若没有成功结果，就以 `failures` 交付。`operation_cancelled` 只在子帧记录且停止令牌收到过请求时记为 `stopped`，否则作为 `error` 留在 `failures` 中。启动阶段若某个分支无法提交，组合会先请求停止并排空已接纳的分支，再抛出原异常。领取赢家值时的移动异常也会从 `co_await` 抛出，此时其他分支已经退出。

代价是每个分支各有一个结果槽和一个根协程帧；胜出后组合仍要等落选分支退出，它们的结果也保留到组合返回。分支至少一个，空参数包无法通过编译。

完整程序见 [进阶竞速](../../examples/coroutine/race_strategies.cpp)。
