# complete_before

要在工作进行到一半时施加截止时间，并且等落选分支清理完毕再继续，用 `complete_before`。它同时等待工作任务和触发任务，返回首个终态，但不会在另一方退出前交付。先包含 `<faio/coroutine/lifecycle.hpp>`。

```cpp
template <class T, class S>
task<deadline_outcome<T>, S> complete_before(task<T, S> work, task<void, S> trigger);
```

`trigger` 是惰性任务，正常完成表示截止到期，抛出异常表示截止机制本身失败。两个任务在同一个任务组中启动，先发布终态的分支获胜，随后向双方请求停止。

返回的 `deadline_outcome<T>` 用 `status()` 描述结果：

| `deadline_status` | 含义 |
| --- | --- |
| `value` | 工作先成功完成，可用 `take_value()` 领取一次结果 |
| `work_error` | 工作先失败 |
| `trigger_error` | 触发任务先失败 |
| `deadline` | 触发任务先成功完成 |
| `stopped` | 某个分支先以协作停止结束 |

`exception()` 返回胜出分支关联的异常，工作和截止正常结束时为空。`take_value()` 只能调用一次，没有工作结果时抛出 `std::logic_error`。

获胜分支发布后会请求另一分支停止，但两个分支都要完成自身清理，`complete_before` 才返回。父级停止请求同样转发给双方，因此截止状态出现不代表借用资源已经归还；复用工作借用的缓冲或句柄前，要先等组合返回。

建立任务组或接纳分支失败时，已接纳的工作会先排空，再传播原异常。每次调用运行两个分支协程，并额外保留一份终态记录。

完整程序见 [停止与截止](../../examples/coroutine/cancellation_deadline.cpp)。
