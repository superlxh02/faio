# on

同一进程里有多个运行时实例，需要把一项任务提交到另一个实例执行并取回结果时，用 `on`。目标实例由 `anchored_runtime` 拥有，通过 `anchor()` 取得准入凭据。先包含 `<faio/runtime/instance.hpp>`。

```cpp
template <class S, class T>
task<T, S> on(basic_runtime_anchor<S> target, task<T, S> child);

class anchored_runtime {
 public:
  explicit anchored_runtime(runtime::config cfg = {});
  basic_runtime_anchor<> anchor() const noexcept;
  void stop(io::shutdown_policy policy = io::shutdown_policy::drain);
};
```

`on` 在目标运行时的上下文中启动 `child`，结果或异常回到来源协程的恢复目标交付。两边的静态调度器类型必须相同。目标必须是 `anchored_runtime`，它固定使用多工作线程模式并拥有独立驱动线程；默认构造的 anchor 没有绑定目标，提交时会被拒绝。

目标实例与来源实例相同时，`on` 直接等待 `child`，不做跨域提交。跨实例时，来源任务不能处于绑核或不可窃取策略下，否则抛出 `std::logic_error`；来源可以是单线程运行时。

目标关闭后，已经保存的 anchor 仍然可以安全使用，`on` 会抛出 `std::logic_error` 表示目标已关闭。`stop()` 关闭准入并按策略排空，不能在运行时 worker 上同步调用。准入登记在目标锁内完成，同时登记的任务寿命保证目标调度器在提交结束前有效。

停止请求沿两条路径进入子任务：来源任务的停止令牌和目标运行时的停止令牌都会转发。子任务仍要协作响应，最终以原值、原异常或 `operation_cancelled` 结束。

跨域调用需要一份完成状态和一个目标根任务，并在来源侧占用一个恢复槽。同实例路径没有这些额外开销。

anchor 只是准入门票，不拥有运行时；`anchored_runtime` 对象本身仍由调用方负责销毁。

跨实例执行目前只在测试里覆盖，可运行用例见 [跨实例调度测试](../../tests/test_coroutine_on.cpp)。
