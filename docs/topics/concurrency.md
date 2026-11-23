# 协程并发

`faio::sync` 和组合器解决两类不同的问题。同步原语保护共享状态、等待条件和消息传递；组合器管理多个任务之间的启动、等待、竞速和收束。同步原语决定谁能碰数据，组合器决定任务何时开始、谁等谁、失败怎样传播。

## 协程同步

同步原语位于 `faio::sync`。等待时挂起当前协程，工作线程可以继续执行其他任务；通知到达后，协程恢复到所属调度域继续运行。互斥锁和信号量按 FIFO 交接等待者；其他原语的唤醒顺序不应当作公平性保证。

等待操作本身可以被取消。阻塞在 `scoped_lock()`、`wait()`、`acquire()` 或 `recv()` 上的协程收到停止请求时，会抛出 `faio::operation_cancelled`。调用方需要保证取消路径上的锁、许可和缓冲区状态仍然正确。

### 互斥锁与条件变量

共享状态用 `faio::sync::mutex` 保护。`scoped_lock()` 返回 RAII 守卫，离开作用域自动解锁：

```cpp
struct Config {
  faio::sync::mutex mutex;
  faio::sync::condition_variable changed;
  std::optional<std::string> value;
};

faio::task<void> publish(Config& config) {
  {
    auto guard = co_await config.mutex.scoped_lock();
    config.value.emplace("catalog");
  }
  config.changed.notify_all();
}

faio::task<void> wait_ready(Config& config) {
  auto guard = co_await config.mutex.scoped_lock();
  co_await config.changed.wait(config.mutex, [&] { return config.value.has_value(); });
  const std::string name = *config.value;
}
```

`condition_variable::wait(mutex, predicate)` 在进入等待前释放锁，被唤醒后重新取得锁，再检查谓词。谓词为假时继续等待，所以通知丢失、虚假唤醒和多个等待者争抢同一个条件都不会破坏循环。

`notify_one()` 只唤醒一个等待者，`notify_all()` 唤醒全部。通知应该在释放锁之后发出：如果持锁通知，被唤醒的任务会立刻阻塞在重新取锁上，增加一次无意义的调度。通知只表示状态可能变化，共享状态的读写仍要放在锁内。

需要手动 `lock()` 时，每条异常路径都必须配对 `unlock()`。能用 `scoped_lock()` 就不要手写，尤其是在 `co_await` 可能抛 `operation_cancelled` 的代码里。

条件变量等待被取消时，会先用不可取消的方式重新取得用户锁，再抛出 `operation_cancelled`。这样调用方在异常路径上仍然持有锁，可以安全地检查或修复共享状态。

### 信号量

限制并发配额时用 `faio::sync::semaphore`。`acquire()` 取得一个计数，`release()` 归还；`acquire_permit()` 返回 RAII 许可，析构时自动归还：

```cpp
faio::task<void> handle_request(faio::sync::semaphore& slots) {
  auto permit = co_await slots.acquire_permit();
  co_await faio::time::sleep(10ms);
}
```

构造时传入初始计数，例如 `faio::sync::semaphore slots{2}` 表示最多两个任务同时进入。`try_acquire()` 不等待，配额不足时返回 `false`；`release(count)` 一次归还多个许可。

信号量只限制数量，不保护信号量之外的数据。共享数据仍要由互斥锁或消息传递保护。许可应该覆盖整个需要限额的临界区；如果只包住创建任务的几行代码，限额就失去意义。

`acquire()` 的等待也可以被取消。拿到许可后要把许可放进 RAII 对象，否则异常路径会泄漏配额。

### 闩与屏障

一次性倒计数由 `faio::sync::latch` 完成。`count_down()` 减一，`wait()` 等到计数归零：

```cpp
faio::sync::latch ready{1};

faio::task<void> initialize() {
  co_await load_config();
  ready.count_down();
}

faio::task<void> use_config() {
  co_await ready.wait();
  // 配置已经就绪
}
```

`latch` 只释放一次，计数归零后不能再重置。它适合一次初始化、多个任务等待的场景；多个等待者可以同时被唤醒。

固定数量的参与者分轮同步时用 `faio::sync::barrier`。每个参与者调用 `arrive_and_wait()`，最后到达的一个释放整轮，然后屏障进入下一轮：

```cpp
faio::sync::barrier phase{worker_count};

faio::task<void> worker(int index) {
  for (int round = 0; round < rounds; ++round) {
    compute_part(index, round);
    co_await phase.arrive_and_wait();
  }
}
```

屏障的参与者数量在构造时确定，不能中途改变。某个参与者提前退出或不再到达，其余参与者会停在某一轮上；为屏障设计任务时，要保证参与者数量稳定，并且退出路径不会跳过 `arrive_and_wait()`。

`latch` 适合只发生一次的初始化信号；`barrier` 适合多轮并行计算，每轮结束后再一起进入下一轮。

### MPSC 通道

任务之间传消息用有界的 `faio::sync::mpsc<T>`。`make(capacity)` 返回发送端和接收端，容量必须大于零：

```cpp
auto [sender, receiver] = faio::sync::mpsc<int>::make(8);

const auto sent = co_await sender.send(1);  // 队列满时挂起
if (!sent) return;                          // 通道已关闭
auto value = co_await receiver.recv();   // 队列空时挂起
```

`send()` 在队列满时等待消费者释放空间，完成结果是 `expected<void>`；通道关闭后返回错误。`try_send()` 不等待，队列满时返回 `false`。`recv()` 在队列空时等待，返回 `expected<T>`；通道关闭且队列排空后返回 `ClosedChannel`。`try_recv()` 暂时为空返回 `nullopt`，与关闭状态分开。

发送端可以复制给多个生产者；接收端同一时刻保留一个未完成的 `recv`。最后一个发送端关闭后，接收端排空队列并观察到关闭。关闭不会丢弃队列中已经入队的数据，消费者仍应把剩余消息读完。

有界通道是背压工具。容量越小，内存占用越低，生产者也越容易停在 `send()` 上；容量越大，突发流量吸收得越多，但消费者崩溃时积压也越多。

### 同步原语的选择

| 需求 | 接口 | 说明 |
| --- | --- | --- |
| 保护一段共享状态的读写 | `mutex` + `scoped_lock()` | 临界区尽量短，不要在持锁时阻塞 |
| 等某个条件成立 | `condition_variable::wait()` | 谓词必须可重复检查 |
| 限制同时执行的任务数 | `semaphore` | 只计数，不保护数据 |
| 等一次初始化完成 | `latch` | 一次性，不可重置 |
| 多轮同步所有参与者 | `barrier` | 参与者数量固定 |
| 在任务之间传消息 | `mpsc<T>` | 有界队列，提供背压 |

多个锁同时出现时，要为所有任务规定一致的加锁顺序，否则可能互相等待。同步原语只在同一调度器的任务之间使用；跨运行时通信走 `on()` 或消息通道，不要直接共享同一个互斥锁。

## 协程并发组合

组合器把多个 `task` 组织成一个新的 `task`。调用 `join(...)`、`select(...)` 或 `scope(...)` 时得到的是一个等待对象；分支要等组合器被 `co_await` 或 `block_on` 驱动时才启动。

### 组合器的共同规则

所有组合器都遵守同一套生命周期约束：

- 创建组合器不会启动分支。它只记录要接纳哪些任务、结果怎样排列，真正的启动发生在等待它的时候。
- 所有分支要使用同一个静态调度器 `S`。需要跨运行时执行时，先用 `on()` 转换，再交给组合器。
- 分支借用调用方的数据时，这些数据要活到组合器返回，返回后不会还有分支在读写它们。
- 组合器返回前会等待所有已接纳的分支退出。胜者已经确定时，落选分支也要完成清理，不会留在后台运行。
- 停止请求是协作式的。组合器会转发父任务的停止请求，也会在需要时请求分支停止；分支是否响应，取决于它有没有在等待点和循环里检查停止令牌。
- 失败处理按组合器分成几类。`join` 和 `join_all` 等全部分支退出后重抛第一个异常；`join_settled` 系列把每项结局收进 `task_outcome`；`select` 等胜者产生后停止落选分支，排空再重抛；`scope` 和 `task_group` 按各自的失败策略收束。

### 组合器一览

| 组合器 | 做什么 | 结果与注意 |
| --- | --- | --- |
| `join(tasks...)` | 等固定数量、类型可以不同的任务 | 按参数顺序返回 tuple；失败要等全部退出后重抛 |
| `join_all(vector)` | 等数量在运行时决定、类型一致的任务 | 按输入下标返回 vector |
| `join_settled(tasks...)` | 等固定数量的异构任务，保留每项结局 | 返回 `task_outcome` 的 tuple |
| `join_all_settled(vector)` | 等动态数量的同类型任务，保留每项结局 | 返回 `task_outcome` 的 vector |
| `join_all_limited(limit, tasks)` | 等全部任务，同时限制在途数量 | 返回 `task_outcome` 的 vector；`limit` 必须为正 |
| `select(tasks...)` | 第一个进入终态的分支胜出 | 返回胜者下标和值；落选分支停止并等待退出 |
| `select_success(tasks...)` | 跳过失败和停止，等第一个成功分支 | 全部失败时返回失败列表 |
| `race_in(group, tasks...)` | 在已有任务组内竞速 | 胜者立即返回，落选分支交给组收尾 |
| `complete_before(work, trigger)` | 工作与触发器竞速，触发器完成表示截止 | 返回 `deadline_outcome` |
| `scope(body)` | 让子任务跟随一段代码的生命周期 | body 退出前排空全部子任务 |
| `task_group(body)` | 在词法范围内动态接纳子任务 | 返回 `group_result`；默认失败即停 |
| `with_stream(factory, body)` | 生产者按消费者节奏交付数据 | 容量为 1，生产者阻塞在 `emit()` |
| `for_each_concurrent(limit, ...)` | 用固定窗口并发处理序列元素 | 在途处理数不超过 `limit` |
| `shared_task` | 多个任务共享一次加载结果 | 工作只启动一次，结果只读 |
| `serial_domain` | 让多项操作跨挂起点互斥 | 域内任务不能等待同一域的其他工作 |
| `on(anchor, task)` | 把任务提交到另一个运行时 | 目标必须是已绑定的 `anchored_runtime` |
| `with_stop_token(token, work)` | 让子任务同时接受父令牌和外部令牌 | 任一来源停止都会向下传播 |
| `with_cleanup(body, cleanup)` | 任务退出后执行异步清理 | 清理在成功、失败、取消时都执行一次 |

### join：固定数量，等全部结束

`join` 接收固定数量、类型可以不同的任务。它启动所有分支，等它们全部退出，再把结果按参数顺序放进 tuple。

```cpp
auto [user, orders] = co_await faio::join(load_user(id), load_orders(id));
```

`task<void>` 没有值，tuple 里保留 `std::monostate` 占位，这样结果下标和参数位置仍然对应。

有一个分支抛异常时，其他分支不会被取消。`join` 等所有分支退出，再按参数顺序重抛第一个异常。需要一个分支失败就尽快停止时，用 `task_group` 的 `fail_fast`，或者在分支里检查停止令牌。

这类组合适合同时读取用户和订单、并行加载多个配置。每个结果都要用，任务之间彼此独立。

### join_all：数量在运行时决定

`join_all` 接收 `std::vector<task<T>>`，任务类型一致，数量在运行时才知道。它返回同长度的 `vector<T>`，下标对应输入顺序。

```cpp
std::vector<faio::task<int>> jobs;
for (int i = 0; i < 8; ++i)
  jobs.push_back(fetch(i));
const auto values = co_await faio::join_all(std::move(jobs));
```

空 vector 直接返回空 vector，不创建任务组。失败处理和 `join` 相同：一个分支失败不会取消其他分支，组合等全部退出后重抛第一个异常。

任务来自循环或网络请求列表时，先用 `push_back` 收集，再 `std::move` 交给 `join_all`。

### join_settled：失败也保留结局

需要保留每个任务的结局时，用 `join_settled`。它把成功值、异常或停止原因装进 `task_outcome<T>`，不把分支异常直接抛给调用方。

```cpp
auto outcomes = co_await faio::join_all_settled(std::move(queries));
for (auto& outcome : outcomes) {
  if (outcome.status() == faio::task_outcome_status::value)
    prices.push_back(outcome.take_value());
}
```

`join_settled(tasks...)` 处理固定数量的异构任务，返回 `task_outcome` 的 tuple；`join_all_settled(vector)` 处理动态数量、类型一致的任务，返回 vector；`settle(task)` 处理单个任务。

`task_outcome` 有三种状态：

| 状态 | 含义 | 读取方式 |
| --- | --- | --- |
| `value` | 正常完成 | `take_value()`，领取一次 |
| `error` | 抛出异常 | `exception()` |
| `stopped` | 协作停止 | `stop_reason()` |

一个分支失败不会让其他分支收到停止请求。父任务停止时，正在运行的子任务会收到停止，因停止退出的项记为 `stopped`。`join_settled` 不抛分支异常；需要把失败重新抛出时，用 `value_or_throw()`。

健康检查、批量导入、价格抓取这类流程，希望拿到每一项的结果再统一处理，用这一组接口。

### select：第一个结束的胜出

`select` 同时启动多个分支，第一个进入终态的分支胜出。终态可以是成功、异常或停止。

```cpp
auto result = co_await faio::select(primary(), fallback());
if (result.index == 0) {
  const int value = std::get<0>(result.value);
  faio::log::logger()->info("primary: {}", value);
} else {
  const int value = std::get<1>(result.value);
  faio::log::logger()->info("fallback: {}", value);
}
```

返回值 `select_result` 记录胜者下标和同形状的 `variant`。`task<void>` 分支对应 `std::monostate`。

胜者以异常结束时，`select` 先请求落选分支停止，等它们全部退出，再重抛胜者异常。落选分支收到的是协作停止请求；如果分支忽略停止、卡在不可取消的操作上，`select` 会一直等下去。

主备地址、两个数据源竞速、先返回先用的场景适合 `select`。需要全部结果时用 `join` 或 `join_all`。

### scope：子任务跟随一段代码

`scope` 把一组子任务限制在 body 的生命周期内。body 接收 `scope_context&`，用 `owner.spawn(task)` 接纳子任务；body 返回时，`scope` 等待所有子任务退出。

```cpp
co_await faio::scope([&](faio::scope_context& owner) -> faio::task<void> {
  owner.spawn(handle_connection(stream_a));
  owner.spawn(handle_connection(stream_b));
  co_await shutdown_signal.recv();
});
// 到这里，两个连接任务都已经退出
```

子任务失败时，`scope` 请求其他子任务停止，排空后把异常抛给调用方。它不会在还有子任务运行时返回。

子任务不能借用 body 返回时销毁的局部变量。需要跨作用域共享的数据放到堆上，或者让它的生命周期覆盖整个 `scope`。

一个连接、一次请求、一段会话内部派生的工作，适合用 `scope` 管理。子任务数量在运行中持续增长时，改用 `task_group`。

### task_group：运行中不断加入任务

`task_group` 在一个 body 的词法范围内动态接纳任务。body 接收 `task_group_context&`，用 `group.spawn_observed(task)` 立即接纳并得到一个 `scoped_join_handle`，再通过 `co_await` 句柄领取 `task_outcome`。

```cpp
auto result = co_await faio::task_group([&](faio::task_group_context& group) -> faio::task<void> {
  auto first = group.spawn_observed(fetch(1));
  auto second = group.spawn_observed(fetch(2));
  const auto a = co_await first;
  const auto b = co_await second;
  // 按 task_outcome 处理 a、b
});
```

body 退出时，组关闭接纳并等待全部子任务。返回值 `group_result<T>` 包含主体终态和子任务摘要：`body` 是主体终态，`children` 按最终接纳顺序保存状态和异常。

默认策略是 `fail_fast`，第一次观察到错误时请求停止；改成 `collect_all` 后，其余子任务继续运行。`close()` 拒绝新任务，`request_stop()` 请求组内任务停止，`stop_token()` 取得这一组独立的停止令牌。

句柄只在 body 的词法范围内有效。body 结束后再等待句柄会抛 `std::logic_error`。丢弃句柄不会停止任务，也不会掩盖异常。父任务的停止请求会传到组内子任务。

### 其他组合器

- `join_all_limited(limit, tasks)`：限制同时在途的任务数量，结果仍按输入下标返回 `std::vector<task_outcome<T>>`。`limit` 必须为正；默认 `fail_fast`，未启动的项记为 `stopped`。
- `select_success(tasks...)`：跳过失败和停止，等第一个成功分支。全部失败时 `winner_index` 为空，`failures` 按输入顺序保留失败信息。
- `race_in(group, tasks...)`：在已有任务组内竞速。胜者一到就返回，落选分支由组统一排空；落选分支后来的错误可以用 `loser_error(index)` 查询。
- `complete_before(work, trigger)`：给工作进行中的阶段加截止时间。`trigger` 正常完成表示截止；返回值的状态包括 `value`、`work_error`、`trigger_error`、`deadline`、`stopped`。
- `with_stream(factory, body)`：生产者按消费者的拉取节奏交付数据，容量为 1。消费者处理不过来时，生产者停在 `emit()`，不会无限堆积。
- `for_each_concurrent(limit, reader_or_factory, operation)`：用固定窗口并发处理序列元素。进行中的 `next()` 也计入窗口，任一项失败会停止其余处理并排空。
- `shared_task`：多个观察者共享一次加载结果。工作只启动一次，`shared_result::value()` 返回借用引用，引用不能越过 `shared_result` 的生命周期。
- `serial_domain`：让多项操作跨挂起点互斥。域内任务不能等待同一域的其他工作，否则会自锁。
- `on(anchor, task)`：把任务提交到另一个运行时执行。目标必须是已绑定的 `anchored_runtime`，来源和目标两侧的静态调度器类型要相同。
- `with_stop_token(token, work)`：让子任务同时接受父任务令牌和外部令牌，任一来源停止都会向下传播。
- `with_cleanup(body, cleanup)`：任务退出后执行异步清理，成功、失败、取消都会执行一次；两侧都失败时抛 `cleanup_error`。

### 怎么选

需要全部结果，用 `join` 或 `join_all`。需要保留每项成功或失败，用 `join_settled` 系列。谁先结束就用谁，用 `select`。子任务的生命周期跟一段代码走，用 `scope`。任务在运行中不断加入，用 `task_group`。限制并发或施加背压，用 `join_all_limited`、`with_stream`、`for_each_concurrent`。

### 常见错误

- 用 `join()` 期待一个分支失败就取消其他分支。它不会；需要停止语义时用 `task_group` 或显式 `request_stop()`。
- 在 `select()` 返回后假设落选分支已经停止。组合会等它们退出，但停止是协作式的；忽略停止的分支会让组合一直等下去。
- 把 `task_group` 的 `scoped_join_handle` 存到 body 之外。句柄只在 body 的词法范围内有效。
- 为了保险无限提高并发上限。先确认下游容量，再用 `join_all_limited()` 或 `with_stream()` 施加背压。
- 只调用 `request_stop()` 就返回，不等待任务收尾。这样无法保证资源已经释放。
- 在 `serial_domain` 的任务里等待同一域的其他工作。域不会重入，等待会自锁。

并发组合的完整语义见[协程与并发](../api/tasks.md)。同步代码怎样接入协程，见[同步与异步桥接](bridging.md)。

[上一章：faio::task\<T\>](task.md) · [下一章：同步与异步桥接 →](bridging.md)
