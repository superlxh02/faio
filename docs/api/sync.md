# 协程同步

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__sync.html)

入口：`<faio/faio.hpp>`。同步原语位于 `faio::sync`，等待时挂起协程，通知后恢复到所属调度域。

## 原语选择

| 类型 | 核心接口 | 场景 |
| --- | --- | --- |
| `mutex` | `lock()`、`unlock()`、`try_lock()`、`scoped_lock()` | 保护共享状态 |
| `semaphore` | `acquire()`、`try_acquire()`、`release()` | 并发配额与限流 |
| `condition_variable` | `wait(mutex, predicate)`、`notify_one()`、`notify_all()` | 等待共享状态满足条件 |
| `latch` | `count_down()`、`wait()` | 一次性倒计数完成 |
| `barrier` | `arrive_and_wait()` | 多个参与者分轮同步 |
| `mpsc<T>` | `make(capacity)`、sender / receiver 端点 | 有界多生产者、单消费者消息队列 |

## 使用 RAII 保护锁

```cpp
// mutex 对象必须覆盖所有等待者的生命周期。
auto guard = co_await mutex.scoped_lock();
// 访问共享状态；guard 离开作用域时归还锁。
```

手动 `lock()` 成功后必须保证所有异常路径都有对应 `unlock()`。`scoped_lock()` 先取得互斥许可再构造 guard，等待失败时不会产生虚假的持锁对象。

条件变量等待期间释放互斥锁，恢复后重新取得；谓词检查应在锁保护下完成。通知本身不替代共享状态同步。

## 有界 MPSC 通道

```cpp
auto [sender, receiver] = faio::sync::mpsc<int>::make(16);
```

发送端可复制，接收端移动独占；容量必须大于零。

| 操作 | 完成结果 |
| --- | --- |
| `co_await sender.send(value)` | `expected<void>`；满队列挂起 |
| `sender.try_send(value)` | `expected<bool>`；满队列为 `false`，关闭为错误 |
| `co_await receiver.recv()` | `expected<T>`；空队列挂起，关闭后报告错误 |
| `receiver.try_recv()` | `expected<optional<T>>`；暂时为空与通道关闭不同 |
| `sender.close()` / `receiver.close()` | 释放相应端点，触发关闭协议 |

单消费者意味着同一时刻只能有一个未完成的 recv。停止、关闭和普通通知均需要唯一恢复权；原语对象不能在等待者仍借用它时销毁。

完整程序：[同步例程](../examples/synchronization.md)。
