# faio::task\<T\>

`faio::task<T>` 是整个 faio 最核心的类型。它把一个协程体包装成惰性、移动独占的任务对象：调用返回任务的函数只建立协程帧，不执行用户代码；`co_await`、`spawn()` 或 `block_on()` 才把任务交给运行时。

## 惰性

```cpp
#include <faio/faio.hpp>

using namespace std::chrono_literals;

faio::task<int> fetch(int id) {
  co_await faio::time::sleep(10ms);
  co_return id * 2;
}

faio::task<void> use_task() {
  auto work = fetch(21);              // 只创建协程帧
  const int value = co_await std::move(work);
  faio::log::logger()->info("value={}", value);
}
```

`fetch(21)` 返回时，协程帧已经建立，协程体停在初始挂起点。直到 `co_await std::move(work)`，`fetch` 才开始执行，并在 `sleep` 完成后交回 `42`。

惰性带来两个直接结果。任务可以像值一样保存、传递、放进容器，执行时机由调用方决定；一个任务对应一次消费，移动之后源对象为空。

调用返回任务的函数时，参数会复制或移动到帧里，函数体的第一条语句还没有执行。`fetch(21)` 可以安全地返回、存进容器、跨函数传递，创建位置不会启动 I/O。

任务帧在首次恢复前由 `task` 独占。移动 `task` 只移动句柄，帧的地址不变；销毁未启动的 `task` 会直接销毁帧，协程体不执行，清理函数也不会运行。

消费任务有三个入口：`co_await` 在当前协程等待结果，`spawn()` 把帧交给运行时并返回 `join_handle`，`block_on()` 在普通线程上驱动事件循环直到任务完成。三者都会让任务离开未启动状态。

任务启动后只执行一次。再次等待同一个任务会在编译期或运行期失败；需要重复观察结果时用 `shared_task`。

## 任务帧与所有权

任务的所有权围绕协程帧展开：

| 阶段 | 帧的所有者 | 可执行的操作 |
| --- | --- | --- |
| 创建、未启动 | `task` 对象 | 移动、销毁、交给 `co_await` 或 `spawn()` |
| 已提交、运行中 | 运行时或 `join_handle` | 等待结果、请求停止 |
| 已完成、未领取 | 结果槽 | 领取一次值，或读取异常 |
| 已领取 | 无 | 任务对象为空，不再可等待 |

`task` 的析构会销毁仍归它所有的帧。未启动的帧可以安全销毁；已经交给运行时或 `spawn()` 的任务不再由 `task` 对象持有，I/O 后端或等待队列可能还借用帧内地址。等待者要等它退出；外部销毁协程帧会让后端持有悬空句柄。

`task` 的复制操作被删除，所有权通过移动转移。一个协程帧只有一个所有者，复制会产生双重等待或双重销毁。

帧里保存参数副本、局部变量、挂起点、结果槽和停止状态。等待方通过结果槽读取返回值或异常，不直接操作帧。任务完成后帧通常保留到结果被领取或句柄销毁，异常和返回值才有稳定的存放位置。

`spawn()` 成功后，`task` 对象变为空，生命周期由运行时和 `join_handle` 共同管理。`join_handle` 析构只释放结果槽，不会停止仍在运行的任务。

跨线程移动 `task` 不会访问协程体，但帧内保存的引用、指针和迭代器仍遵循原来的生命周期。不要借任务的惰性把栈上对象的引用带出作用域。

## 可配置属性

任务在启动前可以附加调度属性。属性通过右值成员函数设置，返回同一个任务。属性属于任务对象，不写进协程体；运行时在首次启动时把它们解析成一份有效策略，之后不再改变。

```cpp
auto unpinned = fetch(1).with_no_stealing();
auto pinned = fetch(2).with_bind_core();
auto observed = fetch(3).with_lifecycle(
    [] { /* 首次恢复前 */ },
    [](const faio::task_exit& exit) noexcept {
      if (exit.status == faio::task_exit::kind::failed) {
        // 只观察失败，不改变结果传播
      }
    });
```

| 属性 | 作用 |
| --- | --- |
| `with_no_stealing()` | 当前任务及后代固定在提交它的 worker 上执行 |
| `with_stealing()` | 请求允许跨 worker 窃取；祖先已禁止时启动告警并保留祖先限制 |
| `with_bind_core()` | 首次执行绑定当前 worker 与已验证的 CPU，同时禁止窃取 |
| `with_no_bind_core()` | 请求不绑核；祖先已经绑核时，该请求不生效 |
| `with_lifecycle(before, after)` | 首次恢复前调用 `before`，任务退出后调用 `after(task_exit)` |

属性只在首次启动时解析一次，挂起恢复不会重新计算。`with_bind_core()` 要求运行时已经通过 `set_worker_cpus()` 配置并验证 CPU 亲和性；当前 worker 没有绑定 CPU 时，启动会抛 `std::logic_error`。

`with_lifecycle` 的回调是同步的，不自动进入阻塞池；任务可能迁移 worker，两次回调不保证在同一线程。`after` 必须 `noexcept`，且恰好调用一次。

这些函数都要求任务是右值，通常在创建后立即链式调用。属性只影响调度和生命周期观察，不改变返回值类型、异常类型或取消语义。

`with_no_stealing()` 与 `with_stealing()` 控制同一组约束；`with_bind_core()` 同时隐含禁止窃取，绑核的协程固定在其 CPU 对应的 worker 上恢复。`with_no_bind_core()` 只在祖先尚未绑核时有效。

`with_lifecycle` 的 `before` 在首次恢复前执行，抛出的异常会让任务以失败状态退出，`after` 仍会收到该状态。`after` 必须 `noexcept`，不应挂起，也不应长时间阻塞 worker。

属性在任务提交后不再生效；需要在 `co_await`、`spawn()` 或 `block_on()` 之前完成配置。

## 属性的继承

子任务默认继承父任务的调度属性，并在这份属性上继续收紧；放宽请求会被拒绝。

规则：

1. 子任务默认继承父任务的调度属性，包括是否允许窃取、是否绑核。
2. 子任务可以继续收紧属性；请求放宽时会被拒绝，运行时记录一次告警，最终沿用父任务已经生效的限制。
3. 收紧只影响子任务自己和它的后代，父任务和兄弟任务不受影响。
4. 组合器、`scope`、`task_group` 的分支按同一套规则继承；跨运行时先用 `on()` 投递，带绑核或不可窃取约束的任务不能直接迁移。

### 规则 1：子任务默认继承父任务属性

父任务允许窃取，子任务默认也允许窃取；父任务禁止窃取，子任务默认也禁止；父任务绑核，子任务默认也在同一个核上恢复。

```cpp
faio::task<void> parent() {
  co_await faio::spawn(child());           // child 继承 parent 的调度属性
  co_await faio::spawn(child().with_no_stealing());
}
```

继承发生在子任务启动时，不是创建 `child()` 的那一刻。创建出来的只是一个惰性任务对象；等到 `spawn()`、`co_await` 或 `block_on()` 启动它时，父任务的调度属性已经确定，子任务才读取这些属性并和自己的设置合并。

### 规则 2：可以收紧，放宽会被拒绝

父任务的调度属性限定了后代能做什么。子任务可以继续加限制，比如把允许窃取改成禁止窃取，或者把未绑核改成绑核。

请求放宽时，运行时不会改变已经生效的策略，只记录一次告警：

- 父任务禁止窃取，子任务调用 `with_stealing()`，最终仍然禁止窃取。
- 父任务已经绑核，子任务调用 `with_no_bind_core()`，最终仍然绑核。

告警只记录一次，组合继续执行；这次请求没有生效。需要放宽限制时，要在父任务上调整，或者换一个不受该限制的任务上下文。

### 规则 3：收紧只向下传递

子任务加了 `with_no_stealing()` 或 `with_bind_core()`，这个限制会继续传给它派生的后代，但不会影响父任务，也不会影响在同一层创建的其他兄弟任务。

```cpp
co_await faio::spawn(worker().with_no_stealing());
co_await faio::spawn(other_worker());   // 仍然使用父任务的策略
```

这条规则让局部代码可以缩小自己的调度范围，不必改动整个调用链。子任务也不能通过收紧来改变父任务的调度方式。

### 规则 4：组合器与跨运行时

组合器创建的分支、`scope` 和 `task_group` 接纳的任务，也按启动时的父任务上下文继承调度属性。

调度器类型一起参与继承。`task<T>` 是 `task<T, default_scheduler>` 的简写；父子任务、组合器和结果句柄要使用同一个 `Scheduler` 类型，类型不同时无法直接组合。

跨运行时组合时，先用 `on()` 把任务提交到目标运行时，再进入组合器。`on()` 的目标必须是已绑定的 `anchored_runtime`，来源和目标两侧的静态调度器类型要相同。来源任务带绑核或不可窃取约束时，`on()` 会拒绝迁移，因为绑核任务固定在其 CPU 对应的 worker 上恢复，禁止窃取的任务依赖当前运行时的调度队列，换一个运行时后这些前提不再成立。

需要在另一个运行时执行时，在目标运行时重新创建任务，再把需要共享的数据通过参数传过去。直接把带约束的任务投递过去会得到拒绝，属性不会被悄悄放宽。

## 对称转移

`co_await` 一个 task 时，faio 默认使用对称转移。父协程在 `await_suspend` 中直接返回子协程句柄，编译器随后恢复子协程，不需要先回到调度器队列：

```cpp
faio::task<int> inner() {
  co_await faio::time::sleep(1ms);
  co_return 42;
}

faio::task<int> outer() {
  const int value = co_await inner();  // 父挂起，子直接在当前位置恢复
  co_return value + 1;
}
```

子协程完成后，`final_suspend` 通过保存的 continuation 恢复父协程。整条链上不会因为每一层 `co_await` 就多一次排队，嵌套等待的栈深和调度开销保持可控。

对称转移的关键在 `await_suspend` 的返回值。返回子协程句柄时，编译器把控制权直接交给子协程，父协程的恢复地址保存在子协程的 continuation 里；子协程到 `final_suspend` 再把控制权交回。整个过程不经过调度队列，也不增加一层回调。

这条路径要求父子任务在同一 worker、同一调度器，并允许直接恢复。等待是否真正挂起由等待器的 ready 路径决定，调用方不需要区分。

对称转移不改变所有权。父协程仍拥有子任务帧，子任务完成后由等待表达式取走结果并销毁帧。

`co_await` 只接受右值 task，左值会编译失败。这个限制让所有权转移出现在类型系统里，避免两个协程等待同一个帧。

`spawn()` 走提交路径：它把任务放进运行时队列并立即返回句柄；`join_handle` 的等待通过结果槽完成，不直接返回子协程句柄。

## 协作取消

任务本身没有强制取消接口。取消通过停止令牌传播：`spawn()` 返回的句柄可以 `request_stop()`，运行中的任务在等待点或检查点观察请求，以 `operation_cancelled` 退出。

```cpp
faio::task<void> worker() {
  auto token = co_await faio::this_coro::stop_token();
  while (!token.stop_requested()) {
    co_await faio::time::sleep(10ms);
  }
}

faio::task<void> stop_worker() {
  auto handle = faio::spawn(worker());
  handle.request_stop();
  co_await handle;  // 等任务退出
}
```

停止是协作式的：`request_stop()` 只发出信号，不会强行中断线程，也不会立即回收协程帧。任务要在循环、等待点或收尾路径里检查令牌；只发请求不等待时，任务可能仍在运行。

停止令牌属于任务上下文。`this_coro::stop_token()` 取得当前任务的令牌；父任务收到停止请求时，子任务通过继承的上下文看到同一个请求。

`request_stop()` 可以重复调用，第一次生效；`stop_requested()` 只读取状态。任务在等待点被取消时通常以 `operation_cancelled` 退出，这个异常和业务异常一样可以被捕获，捕获后仍要完成清理或继续向上传递。

取消的粒度由代码决定。长循环要在每轮检查令牌，阻塞调用要使用支持停止的异步版本，退出路径用 `with_cleanup` 或 `scope` 收尾。

`join()` 等待所有分支结束，某个分支失败不会自动取消其他分支。需要快速失败时用 `task_group` 的 `fail_fast`，或自己请求各句柄停止。

`spawn_detached()` 没有外部句柄，停止请求来自父上下文或任务自己创建的令牌；这类任务要自己处理停止和异常。

## 异常处理

任务体抛出的异常保存在结果槽里，等到 `co_await` 或 `join_handle` 领取结果时按原类型重抛：

```cpp
faio::task<int> parse(bool valid) {
  if (!valid)
    throw std::invalid_argument("invalid input");
  co_return 42;
}

faio::task<void> recover() {
  try {
    const int value = co_await parse(false);
    faio::log::logger()->info("value={}", value);
  } catch (const std::invalid_argument& error) {
    faio::log::logger()->error("parse failed: {}", error.what());
  }
}
```

`parse(false)` 本身不会抛，异常在 `co_await` 时出现。把等待点放进 `try` 块才能接住它。异常只交付给一个等待者；需要多处观察同一结果时，用 `shared_task`。

协程体抛出异常时，promise 捕获它并放进结果槽，协程帧转入完成状态。等待方恢复时重新抛出同一个异常对象，动态类型保持不变，`catch` 可以按原类型匹配。

异常只交付给一个等待者。`join_handle::get()`/`wait()` 也会重抛；`co_await handle` 是协程内的等待方式。多个观察者需要 `shared_task`，它把结果和异常保存在共享状态里。

`join()` 在所有分支退出后按参数顺序选择第一个异常重抛；`join_all` 同样先收束再抛。`settle()`/`join_settled()` 把异常装进 `task_outcome`，调用方用 `status()` 判断，再用 `exception()` 取。

`task<expected<U>>` 中的 `expected` 是普通返回值。任务失败指协程体抛出了异常；I/O 错误通常已经在 `expected` 里。

`spawn_detached()` 没有等待者，异常逃出任务体时没有接收方。任务必须自己捕获并处理，或转成日志和停止。

## 清理函数

任务体退出后需要执行异步清理时，用 `with_cleanup(body, cleanup)`：

```cpp
auto job = faio::with_cleanup(
    [] { return fetch(1); },
    [](const faio::task_exit& exit) -> faio::task<void> {
      co_await flush_log();
      if (exit.status == faio::task_exit::kind::failed)
        co_await report(exit.exception);
    });
const int value = co_await std::move(job);
```

`body` 是返回任务的工厂，`cleanup` 接收 `task_exit` 并返回同一调度器的 `task<void, S>`。清理在任务体退出后执行，正常完成、抛异常、被取消都会走到，且恰好执行一次。清理任务可以挂起并等待异步操作。

`with_cleanup` 返回一个新任务，原任务体被包在组合帧里。组合帧先启动 `body` 工厂并等待它完成，再启动 `cleanup` 工厂；两个工厂都返回同一调度器的任务。

`cleanup` 收到的 `task_exit` 包含 `succeeded`、`failed`、`cancelled` 三种状态和异常指针。它只在清理任务执行期间有效；清理任务派生的后代要在清理返回前排空，否则会留下悬空引用。

清理任务可以 `co_await`，适合刷新缓冲、关闭连接、释放租约。

任务体成功时，清理完成后交付原值；清理失败时改为抛出清理异常。任务体失败时，清理完成后重抛原异常；两边都失败时抛 `cleanup_error`，`primary()` 保存任务体异常，`secondary()` 保存清理异常。任务体被取消时，清理完成后以 `operation_cancelled` 退出。

任务体工厂在启动前抛异常时，清理仍会执行，`task_exit` 为失败状态。

`with_cleanup` 的回调按值移入组合帧。`with_lifecycle` 的 `after` 只做同步观察，异步清理由 `with_cleanup` 负责。

## 常见错误

- 在 worker 线程上调用 `join_handle::get()` 或 `wait()`。这两个接口会阻塞，应在普通线程调用；协程内用 `co_await handle`。
- 对左值 task 直接 `co_await`。等待接口要求右值，需要 `std::move(task)` 明确转交所有权。
- 让 `join_handle` 自然析构，然后以为任务已经取消。析构只放弃结果，任务仍在运行。
- 在 `spawn_detached()` 的任务里让异常逃出。没有等待者会接收它。
- 在子任务上请求放宽父任务的调度约束。启动时会告警，实际策略仍按父任务执行。
- 把 `with_lifecycle` 的 `after` 当成异步清理。它不支持挂起，也不会等待异步操作。
- 把 task 当成可重复等待的 future。每个任务保存一份结果，需要多处观察时用 `shared_task`。
- 在任务帧里保存指向栈上临时对象的引用，任务启动时对象已经销毁。
- 在 `with_lifecycle::after` 里 `co_await`。`after` 是 `noexcept` 同步回调，不支持挂起。

完整的任务接口和生命周期规则见[协程与并发](../api/tasks.md)。下一步看多个任务怎样同步和组合，见[协程并发](concurrency.md)。

[专题首页](index.md) · [下一章：协程并发 →](concurrency.md)
