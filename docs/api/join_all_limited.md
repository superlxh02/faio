# join_all_limited

`join_all_limited` 给动态任务列表加并发窗口，结果仍按输入顺序排列。头文件 `<faio/coroutine/composition.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `join_all_limited(limit, children, options = {})` | 最多同时接纳 `limit` 个任务，按输入顺序返回 `std::vector<task_outcome<T>>` |
| `limit` | 必须为正数，传 0 在调用时抛出 `std::invalid_argument` |
| `group_options.errors` | 默认 `fail_fast`，可改为 `collect_all` 继续接纳后续任务 |
