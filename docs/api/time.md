# 时间与超时

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__time.html)

头文件 `<faio/time/timer.hpp>`。时间接口位于 `faio::time`，时钟为 `std::chrono::steady_clock`，所有等待都响应任务停止请求。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 休眠 | `sleep(duration)` | 挂起指定时长；非正时长立即完成 |
| | `sleep_until(time_point)` | 挂起到单调时钟的指定时间点 |
| 周期定时器 | `interval(period)` | 创建周期定时器，首次 tick 在一个 period 后 |
| | `interval_at(start, period)` | 首次 tick 在 `start + period` |
| | `timer.tick()` | 等待下一次周期触发 |
| | `period()` / `reset()` / `reset_immediately()` / `reset_after()` / `reset_at()` | 查询周期并重置下一次触发 |
| | `set_missed_tick_behavior()` | `Burst` 补发、`Delay` 从当前时间延后、`Skip` 跳到下一自然周期；默认 `Burst` |
| I/O 截止 | `timeout(operation, milliseconds)` | 为受支持的 I/O 等待器设置相对截止时间 |
| | `timeout_at(operation, time_point)` | 设置绝对截止时间 |
