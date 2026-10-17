# 时间与超时

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__time.html)

入口：`<faio/faio.hpp>`，时间接口位于 `faio::time`，基于 `std::chrono::steady_clock`。

## 等待与周期

| 接口 | 用途 / 语义 |
| --- | --- |
| `sleep(duration)` | 挂起指定时长；非正时长立即完成 |
| `sleep_until(time_point)` | 挂起到单调时钟的指定时间点 |
| `interval(period)` | 创建周期定时器；首次 tick 在一个 period 后 |
| `interval_at(start, period)` | 首次 tick 在 `start + period` |
| `timer.tick()` | 等待下一次周期触发 |

```cpp
using namespace std::chrono_literals;
co_await faio::time::sleep(100ms); // 协程内片段
```

这些等待会响应任务停止请求。周期对象支持 `period()`、`reset()`、`reset_immediately()`、`reset_after(duration)` 与 `reset_at(time_point)`。

通过 `set_missed_tick_behavior()` 选择错过 tick 的行为：`MissedTickBehavior::Burst` 补发、`Delay` 从当前时间延后一周期、`Skip` 跳至下一个自然周期点。默认行为为 `Burst`。

## I/O 截止时间

`timeout(operation, milliseconds)` 为受支持的 I/O awaiter 保存相对截止时间；`timeout_at(operation, steady_clock::time_point)` 保存绝对截止时间。

```cpp
// 在持有 stream 与 buffer 的协程内部：
auto result = co_await faio::time::timeout(
    stream.read(buffer), std::chrono::milliseconds{1000});
if (!result) {
  // 检查 result.error()；超时与普通 I/O 失败均需处理。
}
```

相对截止时间从调用 `timeout()` 开始计算，移动 awaiter 不会重置 deadline。右值操作移入返回对象，左值保留借用身份。计时配置本身不会提交 I/O。

`timeout()` 有概念约束，接收的是库支持的 I/O 操作类型；不能假定任意 `task<T>`、`write_all()` 等组合协程都可以直接包入。通用任务竞速可使用 [select](tasks.md)，并在完成后排空失败 / 取消分支。

## 取消与完成

超时不会回滚已经传输的字节。请求停止之后仍需等待操作最终完成，才能释放或复用借用的缓冲区。

[协程竞速例程](../examples/coroutines.md) · [计数器应用中的超时](../examples/counter.md)
