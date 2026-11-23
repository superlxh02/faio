# settle

`settle` 把任务的正常返回、异常或停止收进 `task_outcome`，异常不穿过当前 `co_await`。头文件 `<faio/coroutine/composition.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `settle(task)` | 返回惰性的包装任务，等待时启动子任务并交付 `task_outcome<T>` |
| `status()` | `value`、`error` 或 `stopped` |
| `take_value()` / `exception()` / `stop_reason()` | 领取一次成功值 / 取原异常 / 取停止原因 |
| `value_or_throw()` | 成功返回值，有异常重抛，纯停止抛 `operation_cancelled` |
