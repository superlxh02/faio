# 协程与并发

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__tasks.html)

任务定义和当前协程查询在 `<faio/coroutine/task.hpp>`，并发组合在 `<faio/coroutine/composition.hpp>`，生命周期组合在 `<faio/coroutine/lifecycle.hpp>`。默认运行时的 `block_on`/`spawn` 在 `<faio/runtime/default.hpp>`。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 任务与启动 | `task<T>` | 惰性、移动独占的协程任务；等待或提交才开始执行 |
| | `block_on(task)` | 同步返回结果或传播异常；worker 线程上禁止调用 |
| | `spawn(task)` | 提交独立任务，返回 `join_handle<T>` |
| | `spawn_detached(task)` | 不保留结果句柄；任务必须自行处理异常 |
| | `spawn_blocking(fn)` | 在有界阻塞池执行函数，不保证强制中断 |
| | `join_handle::request_stop()` | 请求协作停止；仍需等待任务退出 |
| 调度属性 | `.with_no_stealing()` / `.with_stealing()` | 禁止或允许跨 worker 窃取，继承后继续收紧 |
| | `.with_bind_core()` / `.with_no_bind_core()` | 绑定或不绑定当前 worker 与 CPU，祖先绑定时不解除 |
| | `with_lifecycle(before, after)` | 一次执行前后调用同步回调；包装任务保持惰性 |
| | `task_exit` | `succeeded`、`failed`、`cancelled` 三种状态，`exception` 保存异常 |
| 并发组合 | `join(tasks...)` | 按输入顺序返回 tuple，等待全部分支 |
| | `join_all(vector)` | 按输入顺序返回 vector，数量运行时决定 |
| | `join_settled(...)` / `join_all_settled(...)` | 每项独立保存 `task_outcome`，单项失败不影响其他分支 |
| | `join_all_limited(limit, tasks)` | 固定窗口限并发，结果仍按输入顺序 |
| | `task_group(options, body)` | 主体内反复接纳子任务，退出前关闭并排空 |
| | `select(tasks...)` | 首个完成者胜出，其余分支停止并等待退出 |
| | `select_success(tasks...)` | 跳过失败与停止，等待第一个成功 |
| | `settle(task)` | 记录单个任务的终态，不向外抛异常 |
| | `with_cleanup(body, cleanup)` | 任务体退出后先执行异步清理，再交付结果 |
| | `with_stop_token(token, work)` | 子任务同时接受父令牌与传入令牌 |
| | `complete_before(work, trigger)` | 工作与触发器竞速，触发器先成功即截止 |
| | `with_stream(factory, body)` | 生产者按需交付，消费者逐项拉取 |
| | `for_each_concurrent(limit, ...)` | 并发处理流元素，在途处理数不超过 limit |
| | `on(anchor, task)` | 投到另一个运行时的锚点上执行 |
| | `scope(body)` | 作用域内派生子任务，退出前排空 |
| | `race_in(group, tasks...)` | 在已有任务组内竞速，可查询落选分支错误 |
| | `serial_domain` / `shared_task` | 串行执行域 / 可多次观察的共享任务 |

## 任务终态与结果类型

`task_outcome<T>` 记录一次执行的终态，不向等待方抛子任务异常。`value` 终态可 `take_value()` 一次；`status()`、`exception()` 和 `stop_reason()` 在领取后仍可查询。

| 类型 | 字段与用法 |
| --- | --- |
| `task_outcome<T>` | `status()` 为 `value`、`error` 或 `stopped`；`value_or_throw()` 折回异常语义 |
| `task_outcome_status` | `value` 可领取值，`error` 取 `exception()`，`stopped` 取 `stop_reason()` |
| `task_stop_reason` | `requested`（已开始执行后收到停止）、`not_started`（未接纳） |
| `group_result<T>` | `body` 为主体终态，`children` 按最终接纳顺序保存子任务状态 |
| `select_result<Ts...>` | `index` 与同形状的 `value` variant |
| `select_success_result` | `winner_index`、`value`，或按输入顺序记录的 `failures` |
| `race_result<Ts...>` | `index`、`outcome` 与 `loser_error()` |
| `deadline_outcome<T>` | `deadline_status`：value、work_error、trigger_error、deadline、stopped |

## 其他接口

| 接口 | 行为 |
| --- | --- |
| `make_awaiter(operation)` | 按值持有操作；等待时调用 `on_suspend`，恢复时取 `on_resume` 返回值 |
| `awaiter_facade<Derived>` | 为操作加成员入口与 `operator co_await`，只消费右值 |
| `this_coro::stop_token()` / `scheduler()` / `worker_id()` | 当前任务的停止令牌、调度器引用、本次恢复所在 worker |
| `this_coro::yield()` / `yield_if_needed()` | 主动重新入队让出执行权 |
