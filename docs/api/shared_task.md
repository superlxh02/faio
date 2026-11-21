# shared_task

按需包含 `<faio/coroutine/composition.hpp>`。

```cpp
auto group.spawn_shared_uncancellable(task<T, S> work)
```

## 语义与限制

组内 eager 工作启动一次。`observe_uncancellable()` 返回拥有只读结果的对象，可以重复观察成功值或异常。单个观察者无法撤回自己的等待。组退出时排空生产者，持有结果对象期间可以借用值。

## 成本

共享结果存储和等待广播事件，使用独立观察任务帧。

R25 的七个独立 macOS Release 进程配对以手写实现的进程 CPU 时间为分母。API 的中位比为 1.1654，单侧 U95 为 1.1866。同一组提前启动一个 move-only 结果，随后借用该结果两次。手写参照用 scope、立即 spawn 与保留只读所有者。Linux ARM64 的四次独立冷进程均测得手写路径每个完整操作 13 次 C++ 分配（4100 B），API 路径 17 次（5524 B）。证据来自 `20261115T002612Z-R25/rich/F05/decision.json`。

可运行程序见 [并发下](../../examples/coroutine/structured_concurrency.cpp)。
