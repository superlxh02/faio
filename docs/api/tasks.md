# 协程与并发

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__tasks.html)

定义任务和查询当前协程时包含 `<faio/coroutine/task.hpp>`，使用并发组合时包含 `<faio/coroutine/composition.hpp>`。核心类型位于 `faio` 命名空间。

## 任务与启动

调用默认运行时的 `block_on`、`spawn`、`spawn_detached` 或 `spawn_blocking` 时包含 `<faio/runtime/default.hpp>`。

| 接口 | 结果 / 行为 | 关键约定 |
| --- | --- | --- |
| `task<T>` | 惰性的、移动独占的协程任务 | 调用函数只创建任务；等待或提交才开始执行 |
| `block_on(task<T>)` | 同步返回 `T` 或传播异常 | 普通线程入口；等待本次根任务组；worker 上禁止调用 |
| `spawn(task<T>)` | `join_handle<T>` | 提交独立并发任务；协程内通过 `co_await` 领取结果 |
| `spawn_detached(task<T>)` | 无结果句柄 | 任务内部处理异常，借用数据必须活到退出 |
| `spawn_blocking(function)` | 可等待的阻塞任务句柄 | 在有界阻塞池执行函数，不能保证强制中断同步函数 |
| `join_handle<T>::request_stop()` | 请求协作停止 | 停止请求不等于已经退出，仍需等待收束 |

## 任务调度属性

任务提供四个无参、右值链式接口：

| 接口 | 本任务的显式请求 |
| --- | --- |
| `.with_no_stealing()` | 禁止窃取 |
| `.with_stealing()` | 允许窃取；不能放宽祖先已经施加的限制 |
| `.with_bind_core()` | 首次启动时绑定当前 worker 及其已验证的 CPU，同时禁止窃取 |
| `.with_no_bind_core()` | 不绑核；不能解除祖先绑定，也不自动允许窃取 |

```cpp
co_await child();                         // 不设置：继承父任务。
co_await child().with_no_stealing();       // 收紧这棵子任务树。
auto result = faio::spawn(child().with_no_stealing());
co_await result;
```

根任务默认可窃取、不绑核。子任务在首次启动时按值继承父任务的有效属性；
`spawn`、`scope`、`join`、`join_all` 和 `select` 的分支也遵守同一规则。
设置只影响当前任务及后代，结束后恢复父任务策略，不改变父任务或兄弟任务。
相同属性的多次链式调用以最后一次显式请求为准，尚未启动就销毁不会打印告警。

子任务可以从可窃取收紧为不可窃取，从不绑核收紧为绑核。若显式允许窃取却继承了
禁止窃取，或显式不绑核却继承了绑核，则在启动时打印 `task scheduling conflict`
告警并保留父约束；每项冲突只诊断一次，恢复后不重复检查。一个任务自行同时请求
绑核和允许窃取，且未继承绑核约束时，启动失败并报告 `std::logic_error`。

不可窃取只禁止其他 worker 从队列领取该任务，不保证始终在同一个 worker：
例如另一个 worker 完成 I/O 后，可以把它投递到那个 worker 的不可窃取队列。
绑核则固定到同一个 worker 和 CPU；后代继承完全相同的目标，不能自行换核。
内置绑核要求 Linux 上已通过 `set_worker_cpus(...)` 配置并验证 worker 的 CPU
亲和性；未配置、普通阻塞池线程或不支持的平台会在任务体执行前拒绝请求。
框架不会在每次任务执行时临时修改共享 worker 的 CPU 亲和性。

```cpp
// Linux：CPU 列表应来自进程允许的 CPU 集合，且数量与 worker 数一致。
// 配置运行时的 set_worker_cpus(...) 后，任务才可请求：
co_await child().with_bind_core();
```

生命周期修饰保留调度请求；请求在包装任务启动时解析一次，再由内部任务继承。
自定义 Scheduler 需要提供 `enqueue_no_stealing(handle)` 和／或
`enqueue_on(handle, worker_id)` 才能接纳相应限制，不支持时明确报错。

内置调度器为每个 local 保存一个容量固定的数组 MPSC 不可窃取队列，本地及远程
提交共用，不另设邮箱。默认容量为 256，可通过
`config_builder::set_no_stealing_queue_capacity(...)` 在启动前配置为 2..2^29 范围内的
2 的幂；运行期间不分配槽位或扩容，也不转入可窃取队列。容量是可接纳的任务／预留
恢复槽数量，索引环使用额外的固定元数据数组。消费时先清空句柄，再归还空闲槽。
任务选择在不可窃取队列与普通来源之间轮转，保留全局队列的定期检查。

受限任务在异步等待登记前预留恢复槽；槽不足时尚未接受等待责任，I/O 返回 `EAGAIN`，
同步等待、sleep 和自定义 awaiter 抛出 `std::overflow_error`，调用者可以处理后重试。
已经挂起的任务使用预留槽恢复，不会在完成时再竞争容量。预留期间也占容量，因此该配置
同时限制受限挂起任务数量；取消且未发布的槽在最后一个令牌释放后归还。
`join/join_all/select` 在启动孩子前预留一个收尾槽，`scope` 预留主体和整组收尾两个槽；
它们也计入容量，保证启动失败时仍能等已接纳的孩子退出。
直接投递的低层 `try_push` 返回 false，已有 void 调度入口以 `std::overflow_error` 报告
无法接纳；满额或高争用下暂时无法领取槽位时立即失败，不原地等待，原句柄仍归提交方。
普通可窃取任务沿用原来的队列路径。

## 任务生命周期策略

`std::move(task).with_lifecycle(policy)` 给一次任务执行配置静态生命周期策略，
也可直接传入 before 和 after 两个可调用对象：

```cpp
auto observed = work().with_lifecycle(
    [] { /* 首次实际执行，inner 尚未启动。 */ },
    [](const faio::task_exit& exit) noexcept {
        if (exit.status == faio::task_exit::kind::failed) {
            // exit.exception 保存原异常；此处只观察，不影响结果传播。
        }
    });
auto result = co_await std::move(observed);
```

策略满足 `task_lifecycle_policy<Policy>`：可移动，提供 `void before()` 和
`void after(const task_exit&) noexcept`。策略及便捷重载中的回调按值保存在
包装协程帧中，通过具体类型静态调用；捕获的引用仍须由调用者保证寿命。

- 包装任务保持惰性。before 在首次恢复时调用，而非创建任务时；未启动就销毁时，
  before、after 均不执行。对空任务配置策略会立即抛出 `std::logic_error`。
- 包装开始执行后，after 恰好调用一次。before 抛异常时，inner 不启动，退出状态为
  `failed`。inner 抛出 `operation_cancelled` 时为 `cancelled`，其他异常为 `failed`；
  两类异常均保存在 `task_exit::exception` 并继续传播。正常返回为 `succeeded`，异常为空。
- after 在 inner 完成并消费其结果后、包装任务最终挂起前执行。把结果写入包装任务
  promise 时若发生移动异常，也报告失败。调用方随后领取包装结果时产生的异常不属于
  此次观察范围。after 必须不抛异常，且不领取或移动任务结果。
- before、after 是同步回调，不自动转入阻塞池。任务可迁移 worker，两次回调不保证
  位于同一线程；它们使用任务执行时恢复的停止令牌、任务组等上下文。
- 停止请求本身不触发 after，也不改变正常返回的成功状态。after 只表示这个任务体
  已退出，不表示 detached 后代已经排空；需要收束后代时使用 `scope` 等结构化操作。

返回类型仍为 `task<T, Scheduler>`，支持 `void`、可移动值及原样返回的 `expected`，
保留静态 Scheduler 类型与显式调度请求。该适配增加一层协程帧及可能的结果移动；
它不是对原任务 `initial_suspend` 或 `final_suspend` 的直接插桩。

C++26 的自动函数注解通过独立 opt-in 组件 `faio::experimental_lifecycle` 提供。给真实协程贴 `[[=faio::experimental::lifecycle<Policy>{}]]` 后，普通调用就会应用策略，无需改变 `co_await` 或 `spawn` 写法；只返回现成 task 的普通转发工厂不会生效。该组件要求同一程序中包含 task 的翻译单元统一启用 C++26 与 `FAIO_EXPERIMENTAL_TASK_LIFECYCLE=1`，并额外拥有一个辅助协程帧。详见 [实验性接口](experimental.md#任务生命周期注解)。

## 并发组合

组合器把多个任务编成一个等待单元。每个接口都有单独的说明页，行为差异集中在两点：结果怎么带回来，以及某个分支失败或停止后其他分支怎么办。

| 接口 | 完成结果 | 行为 |
| --- | --- | --- |
| `join(tasks...)` | 按输入顺序排列的 tuple | 等待全部分支；`void` 对应 `std::monostate` |
| `join_all(vector<task<T>>)` | 按输入顺序排列的 vector | 数量与返回类型在运行时决定 |
| [`join_settled(tasks...)`](join_settled.md) | `tuple<task_outcome<T>...>` | 每项终态独立保存，某项失败不影响其他分支 |
| [`join_all_settled(tasks)`](join_all_settled.md) | `vector<task_outcome<T>>` | 与上一行相同，数量在运行时决定 |
| [`join_all_limited(limit, tasks)`](join_all_limited.md) | `vector<task_outcome<T>>` | 固定窗口限并发，结果仍按输入顺序排列 |
| [`task_group(options, body)`](task_group.md) | `group_result<T>` | 主体内可以多次接纳子任务，退出前关闭并排空 |
| `select(tasks...)` | `select_result` | 首个完成者胜出，随后请求其余分支停止并等待退出 |
| [`select_success(tasks...)`](select_success.md) | `select_success_result` | 跳过失败与停止，等待第一个成功；全失败时按输入顺序收集诊断 |
| [`settle(task)`](settle.md) | `task_outcome<T>` | 记录单个任务的结束方式，不向外抛异常 |
| [`with_cleanup(body, cleanup)`](with_cleanup.md) | body 的类型 | 任务体退出后先执行异步清理，再交付原结果 |
| [`with_stop_token(token, work)`](with_stop_token.md) | `task<T, S>` | 子任务同时接受父令牌与传入令牌的停止请求 |
| [`complete_before(work, trigger)`](complete_before.md) | `deadline_outcome<T>` | 工作与触发器竞速，触发器先成功即判定截止 |
| [`with_stream(factory, body)`](with_stream.md) | body 的类型 | 生产者按需交付，消费者逐项拉取 |
| [`for_each_concurrent(limit, input, op)`](for_each_concurrent.md) | `task<void, S>` | 并发处理流的元素，同时在途的处理数不超过 limit |
| [`on(anchor, task)`](on.md) | `task<T, S>` | 把任务投到另一个运行时的锚点上执行 |
| `scope(body)` | 作用域 body 的完成结果 | 通过作用域对象派生子任务，退出前排空任务组 |

竞速类里还有 [`race_in(group, tasks...)`](race_in.md)：它把竞速放进已有任务组，并在结束后提供接口，查看落选分支之后才发布的错误。任务之间需要排队执行时用 [`serial_domain`](serial_domain.md)；同一份结果要被多次观察时用 [`shared_task`](shared_task.md)。

组合任务本身也是惰性的。将协程任务传给 `join` 不必预先 `spawn`；提前 `spawn` 的任务已经提交，后续等待顺序不会把它们变成串行。

```cpp
// 在协程内部，delayed_value 定义见快速开始。
auto [first, second] = co_await faio::join(
    delayed_value(20), delayed_value(22));
```

## 任务终态与结果类型

并发组合没有统一用异常表达失败。`settle`、`join_settled`、`join_all_limited` 和任务组改用 `task_outcome<T>` 把每个任务的结束方式留在结果里，由调用者自己决定怎么处理。

### task_outcome

`task_outcome<T>` 保存一次完成的记录，只有三种终态：

| 枚举 | 取值 | 含义 |
| --- | --- | --- |
| `task_outcome_status` | `value` | 正常返回，值可以领取一次 |
| | `error` | 抛出异常，异常保存在 `exception()` |
| | `stopped` | 协作停止，`stop_reason()` 给出原因 |
| `task_stop_reason` | `requested` | 任务已经开始执行，运行中收到了停止请求 |
| | `not_started` | 任务从未被接纳，因停止而没有运行 |

```cpp
auto outcome = co_await faio::settle(work());
switch (outcome.status()) {
case faio::task_outcome_status::value:
  use(outcome.take_value());
  break;
case faio::task_outcome_status::stopped:
  // 停止可能没有附带异常，outcome.exception() 可以为空。
  break;
case faio::task_outcome_status::error:
  std::rethrow_exception(outcome.exception());
}
```

- `status()`、`stop_reason()` 和 `exception()` 只读，领取值之后仍然可以查询。
- `take_value()` 只能调用一次，第二次调用或对非 `value` 终态调用都会抛 `std::logic_error`。
- `value_or_throw()` 把三态折回原生协议：有值返回值，有异常重抛，纯停止抛 `operation_cancelled`。
- `task_outcome` 只能移动。值在移动时不会抛异常时直接内联保存，否则单独分配，使结果转交不再移动用户类型。

停止的判定很严格：只有子帧记录的 `operation_cancelled` 且该子任务的停止令牌确实收到了请求，才记为 `stopped`；同样是 `operation_cancelled` 的异常如果来自领取或包装值，则算 `error`。停止请求本身不会让任务提前交付结果。

### 任务组与句柄

`task_group(options, body)` 的主体拿到 `basic_task_group_context<S>&`，可以在词法范围内反复接纳任务：

| 成员 | 用途 |
| --- | --- |
| `spawn_observed(task)` | 接纳并立即投递，返回 `scoped_join_handle<T, S>` |
| `spawn_shared_uncancellable(task)` | 启动共享工作，返回 `shared_task<T, S>` |
| `close()` | 拒绝新的接纳，已有句柄仍可在主体退出前领取 |
| `request_stop()` | 向组内工作请求协作停止 |
| `stop_token()` | 取得该组独立的停止令牌 |

`group_options` 只有一个字段 `errors`：`group_error_policy::fail_fast` 在首次观察到错误时请求停止，`collect_all` 让其他子任务继续跑完。组退出后得到 `group_result<T>`：

- `body` 是主体自己的 `task_outcome<T>`。
- `children` 是按最终接纳顺序排列的 `std::vector<group_child_result>`，每项只含 `index`、`status` 和 `exception`，不持有成功值。

`scoped_join_handle<T, S>` 用于在主体内领取某一项的结果，只能等待一次；主体退出后再等待会抛 `std::logic_error`。丢弃句柄不会取消对应任务，任务组在退出时统一排空。

### 竞速结果

选择器返回的也是结构体，而不是单个值：

| 类型 | 字段 |
| --- | --- |
| `select_result<Ts...>` | `index` 是胜出分支在原始参数中的位置，`value` 是同形状的 `std::variant` |
| `select_success_result<Ts...>` | 成功时填 `winner_index` 与 `value`；全部失败时 `failures` 按输入顺序记录每个分支 |
| `branch_failure` | `index`、`status`、`exception` 和可选的 `stop_reason` |
| `race_result<Ts...>` | `index`、`outcome`（胜出分支的 `task_outcome`）和诊断；`loser_error(branch)` 返回落选分支之后发布的错误，尚未发布时为空 |

相同类型的多个分支在 `select` 和 `select_success` 里靠下标区分，读取 `value` 时要和 `index` 用同一个 variant 下标。

### 截止结果

`complete_before(work, trigger)` 返回 `deadline_outcome<T>`，终态由 `deadline_status` 表示：

| 值 | 含义 |
| --- | --- |
| `value` | 工作先完成并成功 |
| `work_error` | 工作先完成但抛了异常 |
| `trigger_error` | 触发器先完成但抛了异常 |
| `deadline` | 触发器先成功完成，判定为截止 |
| `stopped` | 先发布终态的分支以协作停止结束 |

`status()`、`exception()` 和 `take_value()` 与 `task_outcome` 的用法一致：只有 `value` 终态能领取工作结果，其余状态通过 `exception()` 取原异常。

### 异步序列

`with_stream(factory, body)` 把生产者与消费者的寿命绑在一起。生产者拿到 `stream_producer<T, S>`，每次 `emit(value)` 等到消费者要下一项才继续；消费者拿到 `stream_reader<T, S>`，用 `next()` 拉取，正常结束返回 `std::nullopt`。生产者的异常在消费者下一次 `next()` 时原样抛出。`stream_reader` 不能复制，也不能离开 `with_stream` 的主体。

## 自定义异步等待操作

包含 `<faio/coroutine/awaiter.hpp>`。用户操作只需要提供
`noexcept void on_suspend(Completion)` 和 `on_resume() &&`，无需实现 promise 或额外的 task。

```cpp
struct value_operation {
    int result{};

    template<class Completion>
    void on_suspend(Completion done) noexcept {
        result = 42;
        std::move(done).complete(); // 同步完成同样经过完成协议。
    }

    int on_resume() && noexcept { return result; }
};

// 在已有协程内：
int value = co_await faio::make_awaiter(value_operation{});
```

`make_awaiter` 按值拥有操作，构造时不启动，等待时才调用 `on_suspend`。
`on_resume` 返回什么类型，`co_await` 就返回什么类型：支持 `void`、可移动值和
`expected`；不会自动包装或展开结果。异常从 `on_resume` 传播到等待方。
首版拒绝引用返回，避免直接借用随后销毁的操作内部对象。
`await_operation<Op>` 和 `await_operation_for<Op, Completion>` 可用于检查协议。

需要成员入口时可以继承无状态 `awaiter_facade<Derived>`，钩子协议保持相同：

```cpp
struct signal_operation : faio::awaiter_facade<signal_operation> {
    template<class Completion>
    void on_suspend(Completion done) noexcept { std::move(done).complete(); }
    void on_resume() && noexcept {}
};

co_await signal_operation{}.awaiter();
co_await signal_operation{}; // facade 同时提供 operator co_await。
```

facade 的两个入口只消费右值；命名操作使用 `std::move(operation).awaiter()`。
工厂返回的 `coroutine_awaiter<Op, Scheduler>` 也可以命名后等待，但只能消费一次。
启动前可以通过 `rebind_scheduler<Target>(scheduler_ref_for<Target>)` 转交调度绑定；
faio task 自动按其静态 Scheduler 类型从当前 TLS 选择目标，重绑定不移动稳定状态中的操作，也不创建通知协程帧。

跨线程接入遵循以下合同：

- `Completion` 是只能移动的拥有型完成令牌。回调先保存结果，再调用
  `std::move(done).complete()`；每次接纳必须完成一次。丢弃令牌不会自动取消或唤醒。
- 框架在 `on_suspend` 返回后才允许异步恢复。提前完成由登记线程直接继续；
  后到完成通过捕获的具体调度器入队。调用 `on_resume` 前以 acquire 观察完成，
  与完成发布建立同步，唯一结果写入者无需额外给结果槽加锁。
- 多个终态回调必须在写结果前自行仲裁；完成令牌不能修复已经发生的并发写入。
  完成后不得再访问交还的结果或借用缓冲区，调用方可能立即继续执行。
- 操作对象、完成发布和操作析构由稳定状态及任务组/运行时责任覆盖；
  用户借用的设备、缓冲区等仍必须覆盖真实操作执行期。挂起期间不得从外部销毁父协程。
- `done.stop_token()` 提供父任务的停止令牌。取消由用户操作协作实现；框架不自动调用
  `request_cancel`，也不会在后端尚未排空时提前恢复。
- `on_suspend` 提交失败时仍须保存失败结果或异常并完成；异步恢复入队失败沿用现有
  完成路径的 `std::terminate` 策略，不能从后端的 noexcept 回调向外抛出。

同步完成和外部线程回调的完整程序见 [自定义 awaiter](../../examples/coroutine/custom_awaiter.cpp)。需要执行阻塞函数时，可使用 `spawn_blocking()`。

## 当前协程查询

`faio::this_coro` 查询当前任务的属性，或主动让出执行权：

| 表达式 | 用途 |
| --- | --- |
| `co_await this_coro::stop_token()` | 获取当前任务的停止令牌 |
| `co_await this_coro::scheduler()` | 从当前线程绑定获取匹配任务静态类型的调度器引用 |
| `co_await this_coro::worker_id()` | 查询本次恢复所在 worker，不能当作永久线程身份 |
| `co_await this_coro::yield()` | 主动重新入队，让出执行权 |
| `co_await this_coro::yield_if_needed()` | 根据协作预算决定是否让出 |

## 静态调度任务

所有任务都使用静态调度。`task<T>` 等价于
`task<T, default_scheduler>`，其中 `default_scheduler` 是内置的具体
`domain_scheduler` 类型；`scheduler_ref` 也只是它的单指针静态引用别名。
不存在动态操作表或 `S=void` 调度入口。

`task<T, Scheduler>` 通过 promise 的类型标记保留 Scheduler；任务上下文不保存
调度器实例，也不提供 `task::bind_scheduler()` 预绑定接口。当前线程的
`execution_thread_binding<Scheduler>` 按值保存 `basic_scheduler_ref<Scheduler>`，
查询、让出和等待登记从 TLS 获取目标。等待节点与完成令牌通过 `basic_resume_target<Scheduler>` 保存捕获的具体引用及有效约束，
普通入队、I/O 完成和让出通过模板、CRTP 与 C++23 显式对象参数直接调用调度器。

恢复槽的存储与引用生命周期由 runtime 的 `common/ready_reservation.hpp` 管理，
默认恢复票据声明在 `runtime/common/resume_target.hpp`，预留和投递实现在对应的
`resume_target_impl.hpp`。协程层只使用恢复约定，不包含 MPSC／SCQ 队列定义；
自定义 Scheduler 的恢复目标只保存引用与策略，不携带默认运行时的预留状态。

`runtime::basic_runtime<mode>` 在编译期固定模式，并提供 `scheduler_type`：

```cpp
#include <faio/coroutine/composition.hpp>
#include <faio/coroutine/task.hpp>
#include <faio/runtime/default.hpp>
#include <faio/runtime/instance.hpp>
#include <utility>

using runtime_type = faio::runtime::multi_thread_runtime;
using scheduler_type = runtime_type::scheduler_type;

template<class S>
faio::task<int, S> child(int value) {
    co_await faio::this_coro::yield();
    co_return value;
}

faio::task<int, scheduler_type> work() {
    auto [first, second] = co_await faio::join(
        child<scheduler_type>(20), child<scheduler_type>(22));
    auto blocking = faio::spawn_blocking<scheduler_type>([] { return 1; });
    co_return first + second + co_await std::move(blocking);
}

int main() {
    runtime_type runtime;
    return runtime.block_on(work()) == 43 ? 0 : 1;
}
```

单线程模式使用 `runtime::current_thread_runtime`。两种内置模式共享同一个
具体 `default_scheduler` 和本地快速槽/FIFO/全局队列算法；区别在于一个调用线程
驱动，还是多个后台 worker 驱动。`basic_runtime` 的 `block_on`、`spawn`、
`submit`、`wait_all` 和 `spawn_blocking` 均直接使用这个具体类型。当前线程模式提交的任务由随后的 `block_on` 或
`stop` 驱动；仅对结果句柄调用阻塞 `get()` 不会驱动当前线程运行时。

同步对象使用相应的模板类型，例如 `sync::basic_mutex<Scheduler>`、
`basic_semaphore<Scheduler>`、`basic_condition_variable<Scheduler>`、
`basic_latch<Scheduler>`、`basic_barrier<Scheduler>` 和 `sync::mpsc<T, Scheduler>`。
`scope<Scheduler>(body)` 向 body 传入 `basic_scope_context<Scheduler>&`。
`spawn`、`join`、`join_all` 和 `select` 从输入任务推导调度器类型；空集合的
`join` 可显式写为 `join<Scheduler>()`。

父子任务、结果句柄和同步等待者必须使用同一个 Scheduler 类型，不匹配会在
编译期拒绝。相同 Scheduler 类型的不同实例仍可通过各节点保存的实例指针
互相通知。返回 `task<T>` 的文件、网络和协议组合函数全部默认绑定内置具体
调度器，也属于静态任务链。自定义调度器使用显式的 `task<T, Scheduler>`，
相关辅助协程和同步对象必须传播同一个 Scheduler 类型；驱动线程通过相应的
类型化线程绑定取得目标，默认运行时不接受不匹配的自定义任务。

直接 I/O awaiter、`sleep`、`interval.tick()` 和 `execute` 按任务的静态类型从
当前 TLS 捕获具体调度器，命名左值 I/O 操作仍保留原对象身份。自定义 Scheduler 只需满足
`coroutine_scheduler`，可选 `enqueue_io`、`enqueue_yield` 和库内自让出能力
在编译期选择，缺失时沿原入队路径回退。调度器引用不拥有实例，实例必须存活到
使用它的任务和等待节点全部结束。

任务执行与等待登记需要有效且类型匹配的 TLS 调度绑定，缺失或类型不匹配时
抛出 `std::logic_error`。不能把任务单独绑定到包装调度器，却交给类型不同的
底层 worker 执行。外部原生 awaiter 必须在正确绑定中恢复任务，并遵守捕获的不可窃取／绑核约束；跨线程回调
推荐使用 `make_awaiter`，由完成令牌把任务投回登记时捕获的调度器。连接资源
自身保存的 I/O 域不变，协程在同一运行时的不同 worker 间迁移仍受支持。

静态化针对调度器的入队分派；协程恢复仍遵循标准 `coroutine_handle::resume()`
机制，I/O 后端的既有完成消费者协议保持不变。

调度之外的借用服务也采用静态接口：`task_lifetime_ref` 与任务组完成引用、
worker/目标线程唤醒引用、I/O 完成目标都保存具体类型指针，经模板、CRTP 和显式对象参数调用。
I/O 完成目标借用统一的 `completion_state`，默认运行时直接入队；自定义调度器通过模板通知协程
保留 `S`，不向稳定操作槽保存 `void*` 消费者或发布函数指针。

## 异常与生命周期

任务异常通过 `co_await`、句柄或 `block_on` 向调用者传播。组合操作在传播失败之前收束已经启动的分支，避免分支继续访问被销毁的局部数据。`select` 返回下标仍对应原始参数顺序，重复返回类型也应使用 `std::get<index>` 访问。

从 [任务基础](../../examples/coroutine/task.cpp) 开始，再阅读 [并发上](../../examples/coroutine/composition_basics.cpp)和 [并发下](../../examples/coroutine/structured_concurrency.cpp)。[任务生命周期](../../examples/coroutine/lifecycle.cpp)演示前后回调和异步清理，[示例索引](../../examples/README.md)列出批处理、竞速、取消和异步序列的可运行程序。
