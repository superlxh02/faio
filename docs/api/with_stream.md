# with_stream

按需包含 `<faio/coroutine/sequence.hpp>`。

```cpp
auto faio::with_stream<T, S>(producer_factory, body)
```

## 语义与限制

生产者显式调用 `emit`。消费者同一时刻可以挂起一个 `next`。主体退出时请求停止，等待生产者排空。双方使用同一调度实例。该适配器使用按需拉取协议，生成器的 `co_yield` 语法属于另一套接口。

## 成本

容量一无预取。三个许可通道、一个生产者根。

R25 的七个独立 macOS Release 进程配对以手写实现的进程 CPU 时间为分母。API 的中位比为 1.4200，单侧 U95 为 1.4227。依次拉取八个整数。手写参照使用 scope、请求/产出/消费信号量和一个单值槽。Linux ARM64 的四次独立冷进程均测得手写路径每个完整操作 12 次 C++ 分配（6104 B），API 路径 25 次（8944 B）。证据来自 `20261115T002612Z-R25/rich/F09/decision.json`。

可运行程序见 [异步序列](../../examples/coroutine/sequence.cpp)。
