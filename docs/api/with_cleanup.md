# with_cleanup

`with_cleanup` 在任务体退出后运行一段可挂起的清理逻辑，再交付原结果。头文件 `<faio/coroutine/lifecycle.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `with_cleanup(body, cleanup)` | 等待时调用 body 取得工作任务；任务体退出后调用 cleanup，再交付原结果 |
| `task_exit` | `status` 为 `succeeded`、`failed` 或 `cancelled`，`exception` 保存对应异常 |
| `cleanup_error` | 两侧都失败时抛出，`primary()` 是任务体异常，`secondary()` 是清理异常 |
