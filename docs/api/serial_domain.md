# serial_domain

按需包含 `<faio/coroutine/composition.hpp>`。

```cpp
auto faio::serial_domain<S>::run(factory)
```

## 语义与限制

域内任务从进入到退出保持独占，挂起期间也持有该域。`close()` 封闭接纳并等待已接纳任务退场。域内任务不能同步等待依赖自身释放域的工作。

完整程序见 [串行域](../../examples/coroutine/serial_domain.cpp)。

## 成本

共享控制块、原生协程 mutex 和短接纳锁。

R25 的七个独立 macOS Release 进程配对以手写实现的进程 CPU 时间为分母。API 的中位比为 1.1615，单侧 U95 为 1.2442。两项工作真实 yield 后仍保持独占。手写参照用原生协程 mutex 跨挂起持锁，显式 join 后关闭。Linux ARM64 的四次独立冷进程均测得手写路径每个完整操作 10 次 C++ 分配（3776 B），API 路径 11 次（3936 B）。证据来自 `20261115T002612Z-R25/rich/F11/decision.json`。
