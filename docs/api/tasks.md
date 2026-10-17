# 协程与并发

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__tasks.html)

入口：`<faio/faio.hpp>`。核心类型位于 `faio` 命名空间。

## 任务与启动

| 接口 | 结果 / 行为 | 关键约定 |
| --- | --- | --- |
| `task<T>` | 惰性的、移动独占的协程任务 | 调用函数只创建任务；等待或提交才开始执行 |
| `block_on(task<T>)` | 同步返回 `T` 或传播异常 | 普通线程入口；等待本次根任务组；worker 上禁止调用 |
| `spawn(task<T>)` | `join_handle<T>` | 提交独立并发任务；协程内通过 `co_await` 领取结果 |
| `spawn_detached(task<T>)` | 无结果句柄 | 任务内部处理异常，借用数据必须活到退出 |
| `spawn_blocking(function)` | 可等待的阻塞任务句柄 | 在有界阻塞池执行函数，不能保证强制中断同步函数 |
| `join_handle<T>::request_stop()` | 请求协作停止 | 停止请求不等于已经退出，仍需等待收束 |

## 并发组合

| 接口 | 等待结果 | 行为 |
| --- | --- | --- |
| `join(tasks...)` | 按输入顺序排列的 tuple | 等待全部分支；`void` 对应 `std::monostate` |
| `join_all(vector<task<T>>)` | 按输入顺序排列的结果集合 | 用于运行时决定数量、返回类型相同的任务 |
| `select(tasks...)` | 带 `index` 和 `value` 的结果，`value` 为 variant | 获胜后请求其余分支取消，并等它们退出 |
| `scope(body)` | 作用域 body 的完成结果 | 通过作用域对象派生子任务，退出前排空任务组 |

组合任务本身也是惰性的。将协程任务传给 `join` 不必预先 `spawn`；提前 `spawn` 的任务已经提交，后续等待顺序不会把它们变成串行。

```cpp
// 在协程内部，delayed_value 定义见快速开始。
auto [first, second] = co_await faio::join(
    delayed_value(20), delayed_value(22));
```

## 当前协程查询

`faio::this_coro` 查询当前任务的属性，或主动让出执行权：

| 表达式 | 用途 |
| --- | --- |
| `co_await this_coro::stop_token()` | 获取当前任务的停止令牌 |
| `co_await this_coro::scheduler()` | 获取借用的调度器引用 |
| `co_await this_coro::worker_id()` | 查询本次恢复所在 worker，不能当作永久线程身份 |
| `co_await this_coro::priority()` | 读取任务优先级元数据 |
| `co_await this_coro::yield()` | 主动重新入队，让出执行权 |
| `co_await this_coro::yield_if_needed()` | 根据协作预算决定是否让出 |

## 异常与生命周期

任务异常通过 `co_await`、句柄或 `block_on` 向调用者传播。组合操作在传播失败之前收束已经启动的分支，避免分支继续访问被销毁的局部数据。`select` 返回下标仍对应原始参数顺序，重复返回类型也应使用 `std::get<index>` 访问。

完整可运行代码：[协程基础例程](../examples/coroutines.md)。
