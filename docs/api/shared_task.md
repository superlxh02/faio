# shared_task

同一组里多项任务需要读取同一个只加载一次的结果时，用 `shared_task`。它把一份工作提交到任务组，返回可复制的观察句柄；结果由所有观察者共享，不复制。先包含 `<faio/coroutine/composition.hpp>`。

```cpp
template <class S = default_scheduler>
class basic_task_group_context {
 public:
  template <class T>
  auto spawn_shared_uncancellable(task<T, S> child);  // 返回 shared_task<T, S>
};

template <class T, class S = default_scheduler>
class shared_task {
 public:
  task<shared_result<T, S>, S> observe_uncancellable() const;
};

template <class T, class S = default_scheduler>
class shared_result {
 public:
  std::exception_ptr exception() const noexcept;
};
```

`spawn_shared_uncancellable` 立即把工作提交到组里，工作只启动一次。返回的 `shared_task` 是轻量句柄，可以复制给多个子任务。每个观察者调用 `observe_uncancellable()` 等待生产完成，取得自己的 `shared_result`。

`observe_uncancellable()` 的等待不能由单个观察者单独取消。生产者仍受任务组的停止策略约束；组请求停止时，工作按协作规则退出，观察者随后在结果上看到对应的异常或取消。

`shared_result` 持有结果状态的共享所有权。`value()` 返回结果引用，不复制结果；生产失败时每次调用都重新抛出保存的异常。`exception()` 直接返回异常指针，成功时为空。`value()` 返回的引用只在 `shared_result` 存活期间有效。`T` 为 `void` 时只表示工作已完成，没有业务值。

组退出时先排空生产者，已经取得的 `shared_result` 仍可继续读取。这种共享方式适合只读配置、一次性加载的句柄或较大的 move-only 结果。

代价是每个观察者各占一个等待协程帧，结果本身只存一份；共享状态和完成通知比普通子任务多一层间接。

完整程序见 [并发下](../../examples/coroutine/structured_concurrency.cpp)。
