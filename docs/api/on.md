# on

按需包含 `<faio/runtime/instance.hpp>`。

```cpp
task<T, default_scheduler> faio::on(basic_runtime_anchor<default_scheduler> target,
                              task<T, default_scheduler> work);
```

## 语义与限制

同一个静态调度器类型下，工作在目标实例启动，结果通过原生恢复目标交还来源实例。目标由 `anchored_runtime` 持有，只有其创建的 anchor 能提交。外壳关闭后，已保存的 anchor 仍可安全拒绝。目标需要实际驱动的多 worker 运行时。来源可为单线程运行时。同实例直接等待，跨实例的绑核和不可窃取来源拒绝。

## 成本

跨域使用一份完成状态、一个目标根和现有来源恢复槽。源与目标分别保持真实排空责任。相同实例无需另行提交。

R25 的七个独立 macOS Release 进程配对以手写实现的进程 CPU 时间为分母。API 的中位比为 1.1026，单侧 U95 为 1.1119。current 来源驱动提交到已运行的单 worker 目标，再在来源消费结果。手写参照提交到同一原生目标 runtime 并等待结果。Linux ARM64 的四次独立冷进程均测得手写路径每个完整操作 5 次 C++ 分配（1376 B），API 路径 8 次（2088 B）。证据来自 `20261115T002612Z-R25/rich/F07/decision.json`。
