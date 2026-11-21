# for_each_concurrent

按需包含 `<faio/coroutine/sequence.hpp>`。

```cpp
task<void, S> faio::for_each_concurrent<T, S>(std::size_t limit, stream_factory, operation_factory)
```

## 语义与限制

若一次 `next()` 正在挂起，窗口为它预留一个槽。处理失败会停止后续工作并排空。工厂在词法作用域内按值持有，并发调用时需要遵守共享状态的同步契约。

## 成本

只计生命周期的工作根与固定并发许可。不累积全流结果。

R25 的七个独立 macOS Release 进程配对以手写实现的进程 CPU 时间为分母。API 的中位比为 1.1508，单侧 U95 为 1.5627。处理八项输入，并发窗口为二。手写参照采用容量一、按需推进的生产者及两个滚动 join 句柄。Linux ARM64 的四次独立冷进程均测得手写路径每个完整操作 38 次 C++ 分配（9944 B），API 路径 42 次（15688 B）。证据来自 `20261115T002612Z-R25/rich/F13/decision.json`。

可运行程序见 [异步序列](../../examples/coroutine/sequence.cpp)。
