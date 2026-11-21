# race_in

按需包含 `<faio/coroutine/composition.hpp>`。

```cpp
auto race_in(basic_task_group_context<S>& group, task<Ts, S>... children)
```

## 语义与限制

首终态就绪后，调用方可以在同一词法组中继续执行。落选工作会收到协作停止请求，组退出时完成排空。通过 `loser_error(i)` 查询已记录的迟到异常。被工作借用的资源必须活到组退出。

完整程序见 [进阶竞速](../../examples/coroutine/race_strategies.cpp)。

## 成本

一个拥有型竞速状态，分支通过只计生命周期的接纳登记。

R25 的七个独立 macOS Release 进程配对以手写实现的进程 CPU 时间为分母。API 的中位比为 1.0064，单侧 U95 为 1.1727。两路工作，先向调用方交付首个结果，再停止并排空另一条。手写参照用 scope、信号量和独立停止源。Linux ARM64 的四次独立冷进程均测得手写路径每个完整操作 13 次 C++ 分配（5360 B），API 路径 14 次（5704 B）。证据来自 `20261115T002612Z-R25/rich/F06/decision.json`。
