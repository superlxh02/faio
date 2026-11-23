# task_group

`task_group` 在协程内部动态派生一组子任务，离开作用域前等它们全部退出。头文件 `<faio/coroutine/composition.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `task_group(body)` / `task_group(options, body)` | 主体拿到 `basic_task_group_context<S>&`，退出时关闭接纳并排空全部已接纳任务 |
| `group_options.errors` | 默认 `fail_fast`，可改为 `collect_all` 让其余子任务继续运行 |
| `spawn_observed(task)` | 立即接纳并投递，返回可领取一次 `task_outcome` 的 `scoped_join_handle` |
| `spawn_shared_uncancellable(task)` | 启动共享工作，可多次观察 |
| `close()` / `request_stop()` / `stop_token()` | 拒绝新任务 / 请求协作停止 / 取得组独立停止令牌 |
| `group_result<T>` | `body` 是主体终态，`children` 按最终接纳顺序保存状态与异常 |
