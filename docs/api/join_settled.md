# join_settled

`join_settled` 等齐固定数量的异构任务，按参数顺序保留每项结局；一项失败不会让其他任务提前结束。头文件 `<faio/coroutine/composition.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `join_settled(children...)` | 并行启动全部子任务，按参数顺序返回 `std::tuple<task_outcome<T>...>`；`void` 子任务保留位置但没有值 |
| `task_outcome<T>` | 每槽独立保存 `value`、`error` 或 `stopped` |
| `take_value()` | `value` 终态可领取一次 |
| `exception()` / `stop_reason()` | 读取原异常或停止原因 |
