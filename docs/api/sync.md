# 协程同步

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__sync.html)

同步原语位于 `faio::sync`。等待时挂起协程，通知后恢复到所属调度域。互斥锁与条件变量使用 `<faio/sync/mutex.hpp>`，信号量、闩和屏障使用 `<faio/sync/coordination.hpp>`，MPSC 通道使用 `<faio/sync/channel.hpp>`。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 同步原语 | `mutex` | `lock()`、`unlock()`、`try_lock()`、`scoped_lock()`；保护共享状态 |
| | `condition_variable` | `wait(mutex, predicate)`、`notify_one()`、`notify_all()`；等待期间释放锁 |
| | `semaphore` | `acquire()`、`try_acquire()`、`release()`；并发配额与限流 |
| | `latch` | `count_down()`、`wait()`；一次性倒计数 |
| | `barrier` | `arrive_and_wait()`；多个参与者分轮同步 |
| | `mpsc<T>::make(capacity)` | 有界多生产者单消费者通道；容量必须大于零 |
| MPSC 操作 | `sender.send(value)` | 队列满时挂起，完成结果为 `expected<void>` |
| | `sender.try_send(value)` | 队列满返回 `false`，通道关闭返回错误 |
| | `receiver.recv()` | 队列空时挂起，完成结果为 `expected<T>` |
| | `receiver.try_recv()` | 暂时为空返回 `nullopt`，与通道关闭不同 |
| | `sender.close()` / `receiver.close()` | 释放端点并触发关闭协议 |
