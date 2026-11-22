# task_group

需要在协程内部动态派生一批子任务，并确保离开作用域前它们全部退出时，用 `task_group`。组的主体在词法范围内接纳任务，退出时关闭接纳并排空全部已接纳任务。先包含 `<faio/coroutine/composition.hpp>`。

```cpp
template <class S = default_scheduler, class Body,
          class Work = std::invoke_result_t<Body&, basic_task_group_context<S>&>,
          class T = typename Work::value_type>
  requires std::same_as<Work, task<T, S>>
task<group_result<T>, S> task_group(group_options options, Body body);

template <class S = default_scheduler, class Body>
auto task_group(Body body);
```

第一个重载接受 `group_options`，第二个使用默认的 `fail_fast` 策略。`Body` 必须能以 `basic_task_group_context<S>&` 调用并返回 `task<T, S>`；等待 `task_group` 返回的任务时主体才开始运行。

主体通过上下文操作组：

| 成员 | 用途 |
| --- | --- |
| `spawn_observed(task)` | 立即接纳并投递子任务，返回 `scoped_join_handle<T, S>` |
| `spawn_shared_uncancellable(task)` | 启动共享工作，可多次观察，观察等待不可单独取消 |
| `close()` | 拒绝新任务，已有句柄在主体退出前仍可领取 |
| `request_stop()` | 向组内工作请求协作停止 |
| `stop_token()` | 取得组独立的停止令牌 |

`scoped_join_handle` 用 `co_await` 领取一次 `task_outcome<T>`。组返回的 `group_result<T>` 包含主体终态 `body`，以及按最终接纳顺序排列的 `children`；后者只保存状态和异常，不保存成功值。子任务失败不会从 `task_group` 直接抛出，由调用方从句柄或摘要中处理。`group_options` 的 `errors` 字段默认为 `fail_fast`，首次观察到错误会请求停止；改成 `collect_all` 后其余子任务继续运行。`operation_cancelled` 沿用 `settle` 的判定：子帧记录且停止令牌收到过请求才算 `stopped`。

主体返回后组关闭接纳并等待所有已接纳任务退出，然后才交付结果。句柄只在主体词法范围内有效，主体结束后再等待会抛 `std::logic_error`；丢弃句柄不会停止任务，也不会掩盖异常。子任务不能借用主体返回时销毁的局部值，被借用的数据要一直有效到任务组返回。父任务的停止请求会传到组内子任务。

代价是每个被接纳的任务各有一个根协程帧和结果槽，组还要保留接纳记录直到返回。

完整程序见 [并发下](../../examples/coroutine/structured_concurrency.cpp)。
