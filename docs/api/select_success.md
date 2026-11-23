# select_success

`select_success` 等第一个成功分支；失败和停止的分支不参与胜出。头文件 `<faio/coroutine/composition.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `select_success(tasks...)` | 并行启动所有分支，第一个成功分支胜出，随后停止其余分支并等它们退出 |
| `winner_index` | 成功时胜出分支在原始参数中的位置 |
| `value` | 同形状的 `std::variant`，按 `winner_index` 读取；`void` 对应 `std::monostate` |
| `failures` | 全部失败或停止时按输入顺序记录每个分支的 `branch_failure` |
