# settle

协程里经常需要知道一个任务究竟正常返回、抛出异常，还是被协作停止，又不希望异常直接穿过当前的 `co_await`。`settle` 把一次执行的结局收进 `task_outcome<T>`，由调用方按状态处理。先包含 `<faio/coroutine/composition.hpp>`。

```cpp
template <class T, class S>
task<task_outcome<T>, S> settle(task<T, S> child);
```

`settle` 返回惰性的包装任务，不等待它，子任务不会启动。等待时子任务沿用当前协程的上下文，因此调用方的停止请求会照常传到子任务；`settle` 不会因为收到停止而提前交付结果，只有子任务真正退出、协程帧被回收之后才有终态。

判断 `stopped` 的条件比较严格：子帧记录了 `operation_cancelled`，并且该子任务的停止令牌确实收到过请求，才记为 `stopped`。如果 `operation_cancelled` 来自领取或包装返回值的过程，即使停止请求已经发出，也仍然记为 `error`。这条规则无法区分主动抛出的 `operation_cancelled` 与真正的停止；业务上需要区分时，应让子任务使用自己的错误类型。

```cpp
auto outcome = co_await faio::settle(fetch_quote());
if (outcome.status() == faio::task_outcome_status::value)
  total += outcome.take_value();
else if (outcome.status() == faio::task_outcome_status::stopped)
  ++cancelled;
```

`task_outcome` 只能移动，成功值只能领取一次；`status()`、`exception()` 和 `stop_reason()` 在领取之后仍可查询。想把三态折回异常语义时用 `value_or_throw()`，纯停止且没有原异常时会抛出 `operation_cancelled`。包装帧分配失败会直接从调用 `settle` 的位置抛出。

代价是每次调用增加一层包装协程帧。成功值在移动可能抛异常时单独分配存放，之后的转交不再移动用户类型。

完整程序见 [批处理与任务终态](../../examples/coroutine/batch_results.cpp)。
