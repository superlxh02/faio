# complete_before

`complete_before` 给工作进行中的阶段加截止时间，等落选分支清理完毕再返回。头文件 `<faio/coroutine/lifecycle.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `complete_before(work, trigger)` | 同时等待工作任务和触发任务，返回首个终态；`trigger` 正常完成表示截止 |
| `deadline_outcome<T>::status()` | `value`、`work_error`、`trigger_error`、`deadline` 或 `stopped` |
| `take_value()` / `exception()` | 仅 `value` 可领取一次工作结果 / 取胜出分支关联的异常 |
