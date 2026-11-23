# shared_task

`shared_task` 把只运行一次的工作交给多个观察者共享。头文件 `<faio/coroutine/composition.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `spawn_shared_uncancellable(task)` | 立即把工作提交到任务组，返回可复制的 `shared_task` 句柄 |
| `shared_task::observe_uncancellable()` | 每个观察者等待生产完成并取得自己的 `shared_result` |
| `shared_result::value()` / `exception()` | 返回结果引用，不复制；失败时每次调用重新抛出保存的异常 |
