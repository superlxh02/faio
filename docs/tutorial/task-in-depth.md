# 深入 task

前面几章我们一直在用 `task`。这一章把盖子掀开，看看一个任务从创建到完成经历了什么：`co_await` 背后那三个方法分别做什么，`this_coro` 提供了什么上下文，以及取消信号是怎么传到任务里的。

## 任务是怎么启动的

先看一段能观察启动时机的代码：

```cpp
faio::task<int> direct_quote(bool& started) {
  started = true;
  co_await faio::this_coro::yield();
  co_return 21;
}

faio::task<void> show_lazy() {
  bool started = false;
  auto pending = direct_quote(started);
  if (started)
    throw std::logic_error("创建任务时不应执行任务体");

  const int price = co_await std::move(pending);
  if (!started || price != 21)
    throw std::logic_error("直接等待未交付预期报价");
}
```

`direct_quote(started)` 调用之后，`started` 还是 `false`。这印证了 `task` 的惰性：调用协程函数会做实参求值、分配协程帧、构造 promise，但函数体一行都没跑。

`co_await std::move(pending)` 才真正启动它。具名任务必须 `move`，因为 `task` 禁止复制，等待权只有一份。临时任务就不用：`co_await direct_quote(started)` 直接启动。

任务还是移动独占的。这背后有个实际约束：一旦任务启动并被某个 I/O 操作借用了协程帧里的地址，就不能从外部强行销毁它——后端可能还在往那块内存写结果。未启动的任务可以安全丢弃；已启动的任务必须等到它自己结束。

## `co_await` 背后的三个方法

任何能写在 `co_await` 后面的对象都叫可等待对象（awaitable）。编译器会调用它的三个方法：

| 方法 | 作用 |
| --- | --- |
| `await_ready()` | 问"已经准备好了吗"。返回 true 就跳过挂起，直接取结果 |
| `await_suspend(handle)` | 没准备好时调用，登记当前协程并决定是挂起还是转移执行权 |
| `await_resume()` | 协程恢复后调用，取得结果，或者在这里抛出异常 |

`task`、`read()`、定时器、同步原语的等待，都通过各自的 awaiter 实现这套协议。

faio 还提供一个两阶段的辅助基类 `awaiter_facade`，让你不必手写上面三个方法：

```cpp
struct answer_operation : faio::awaiter_facade<answer_operation> {
  int value{};

  template <class Completion>
  void on_suspend(Completion done) noexcept {
    value = 42;
    std::move(done).complete();
  }

  int on_resume() && noexcept { return value; }
};

faio::task<int> answer() {
  co_return co_await answer_operation{};
}
```

规则是这样的：

- `on_suspend` 必须是 `noexcept`，返回 `void`。它启动异步操作，并拿到一个完成回调。
- `on_resume() &&` 交付结果，可以返回值、抛异常，或者返回 `void`。
- 操作完成后，先存好结果、结束资源借用，再**恰好一次**调用 `std::move(done).complete()`。之后不要再碰那个操作的状态——它可能已经被消费或释放了。

完整代码见 [`examples/coroutine/custom_awaiter.cpp`](../../examples/coroutine/custom_awaiter.cpp)。

## `this_coro` 提供什么

在任务体里，`co_await faio::this_coro::xxx` 可以拿到当前协程的上下文：

```cpp
const auto token = co_await faio::this_coro::stop_token();   // 停止令牌
auto scheduler = co_await faio::this_coro::scheduler();       // 调度器引用
auto worker = co_await faio::this_coro::worker_id();          // 当前 worker 编号

co_await faio::this_coro::yield();                            // 主动让出执行权
co_await faio::this_coro::yield_if_needed();                  // 按协作预算决定让不让
```

`worker_id()` 反映的是本次恢复在哪个 worker 上，**不能**当作永久的线程身份——异步完成后的恢复可能换一个 worker。`yield_if_needed()` 依据协作预算决定是否重新入队，适合放在 CPU 循环里，让别的任务也有机会跑。

## 协作取消

取消在 faio 里是"请求"，不是"强杀"。外部通过 `join_handle::request_stop()` 提出请求，任务通过停止令牌自己决定何时退出：

```cpp
faio::task<void> work_until_stopped(std::stop_token& observed,
                                    faio::sync::latch& entered,
                                    faio::sync::latch& pending) {
  observed = co_await faio::this_coro::stop_token();
  entered.count_down();
  co_await pending.wait();   // 收到停止请求后，这里抛出 operation_cancelled
}

faio::task<void> stop_it() {
  std::stop_token observed;
  faio::sync::latch entered{1};
  faio::sync::latch pending{1};
  auto job = faio::spawn(work_until_stopped(observed, entered, pending));

  co_await entered.wait();       // 等工作真正拿到令牌
  job.request_stop();            // 发出请求
  try {
    co_await job;
  } catch (const faio::operation_cancelled&) {
    // 任务已按协作方式退出
  }
}
```

几个容易误解的地方：

- `request_stop()` 返回时任务**未必已经退出**。要拿结果，还得 `co_await job`。
- `yield_if_needed()` 不会自动抛取消异常。纯计算循环里必须主动检查 `token.stop_requested()`。
- 销毁 `join_handle` 不会取消任务。想停就显式 `request_stop()`。
- 同步原语的等待（如 `latch::wait()`）在收到停止时会抛 `operation_cancelled`；I/O 通常通过 `expected` 报告取消错误。

完整例子见 [`examples/coroutine/cancellation_deadline.cpp`](../../examples/coroutine/cancellation_deadline.cpp)。

## 任务的调度属性

任务启动时继承父任务的有效属性，也可以通过右值链式接口提出更严格的约束：

```cpp
co_await child().with_no_stealing();
auto handle = faio::spawn(child().with_bind_core());
```

| 接口 | 含义 |
| --- | --- |
| `.with_no_stealing()` | 禁止其他 worker 窃取本任务 |
| `.with_stealing()` | 请求允许窃取，但不能放宽祖先的禁止约束 |
| `.with_bind_core()` | 启动时绑定当前 worker 及已验证的 CPU，同时禁止窃取 |
| `.with_no_bind_core()` | 请求不绑核，不能解除祖先绑定，也不会自动允许窃取 |

属性只作用于当前任务及其后代。绑核在 Linux 上要求先用 `config_builder::set_worker_cpus(...)` 配置并验证 CPU 亲和性，否则会在任务体启动前报错。

## 运行看看

本章涉及的三个例子都可以单独编译运行：

```sh
cmake --build build
./build/my_app
```

## 下一步

现在你知道取消是怎么工作的了。但取消只是并发控制的一部分：如果多个分支在赛跑，谁先完成就算谁赢；如果还带一个截止时间，逻辑会更复杂。下一章我们看 `select`、`race_in` 和 `complete_before`，把这些场景理清楚。见[select 与并发组合](select.md)。

---

[上一章：分帧](framing.md) · [下一章：select 与并发组合 →](select.md)
