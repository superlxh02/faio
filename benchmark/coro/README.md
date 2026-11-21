# 协程与帧分配基准

**简体中文** | [English](README_EN.md)

`faio_coro_benchmark.cpp` 与 `tokio-benchmark/` 对照任务、协程让出、同步原语和并发组合。两侧校验相同工作流的结果和实际操作数。

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release --target faio_coro_benchmark -j4
./build/linux-main-release/benchmark/faio_coro_benchmark 100000 batch
./build/linux-main-release/benchmark/faio_coro_benchmark 100000 sampled
cargo build --manifest-path benchmark/coro/tokio-benchmark/Cargo.toml --release --locked
```

| 计量模式 | 输出 | 含义 |
| --- | --- | --- |
| batch | ns/op、ops/s | 总耗时除以实际操作数，无逐操作读时钟 |
| sampled | 平均时延及 P50/P90/P99 等 | 逐操作采样，包含读时钟与记录成本 |

同一场景的操作单位可能是一次 yield、一条消息、一个往返或一组 32 个任务。不同单位的成本不能直接合并成总体排名；`timer_calibration` 是计时参考，不作为运行时胜负指标。

主要工作流包括：

- ready task、yield、spawn/join 和普通线程 block_on 入口。
- 同 worker 及跨 runtime 交接、外部通知。
- 无争用和有争用的 mutex、semaphore、condition_variable、latch 及 barrier。
- MPSC 立即完成、背压和多生产者传递。
- join、join_all、scope 及 select 的启动、汇合和排空。

Tokio 的条件变量与 latch 对照使用 Notify 及计数器组合，scope 使用 JoinSet 组合。它们只作成功工作流参考，不表示接口语义完全相同。Rust ready future 可能内联，C++ 惰性 task 通常拥有独立协程帧。

`current_thread_benchmark.cpp` 提供单线程驱动与单后台 worker 对照。`frame_allocation_benchmark.cpp` 使用独立工厂测量小、中、大帧及突发分配；缓存与全局 new 分别构建，关闭 LTO。全局 new 可通过 Linux 分配器替换与 tcmalloc 对照。

worker 数量、CPU 集合、编译器及休眠策略都影响结果。比较时记录实际配置，预热与多轮测量分开，保存全部数据与波动。总览见[基准说明](../README.md)。
