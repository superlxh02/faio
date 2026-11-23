# join_all_settled

`join_all_settled` 处理数量在运行时确定、类型一致的任务，并按输入下标保留每项结局。头文件 `<faio/coroutine/composition.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `join_all_settled(children)` | 并行启动 vector 中的全部任务，按输入下标返回同长度的 `std::vector<task_outcome<T>>` |
| `task_outcome<T>` | 每槽独立保存 `value`、`error` 或 `stopped`，失败或停止的项保留原下标 |
| `take_value()` / `exception()` | 领取一次成功值 / 取原异常 |
