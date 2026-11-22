# 协程与帧分配基准

**简体中文** | [English](README_EN.md)

`faio_coro_benchmark.cpp` 和 `tokio-benchmark/` 跑同一组工作流：就绪任务、协程让出、spawn/join、同步原语和结构化并发。两侧都会校验结果和实际操作数，因此对照的始终是同一份负载。

## 构建与运行

CMake 下只构建协程基准：

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release --target faio_coro_benchmark -j4
./target/cmake-build-linux-main-release/benchmark/faio_coro_benchmark 100000 batch
./target/cmake-build-linux-main-release/benchmark/faio_coro_benchmark 100000 sampled
cargo build --manifest-path benchmark/coro/tokio-benchmark/Cargo.toml --release --locked
```

源码放在 `benchmark/coro/`，但基准目标统一注册在上一层。Bazel 里没有 `benchmark/coro` 这个包，target 属于 `//benchmark`：

```sh
bazel run --config=faio_headers //benchmark:faio_coro_benchmark -- 100000 sampled
```

xmake 用 target 名区分，不需要额外路径：

```sh
xmake f -c -y -m release --faio_benchmarks=y
xmake run faio_coro_benchmark 100000 sampled
```

三套构建里的协程基准都按头文件目标编译，不需要启用具名模块。帧分配程序是独立的 target，调用方式见下文。

## 两种计量模式

| 计量模式 | 输出 | 含义 |
| --- | --- | --- |
| `batch` | ns/op、ops/s | 总耗时除以实际操作数，不逐操作读时钟 |
| `sampled` | 平均时延及 P50/P90/P99 等 | 逐操作采样，包含读时钟与记录成本 |

同一场景里的一次操作可能是让出一次、传一条消息、完成一个往返或跑完一组 32 个任务。单位不同，不能把各行的纳秒数直接相加或合并成总体排名。`timer_calibration` 只用于校准计时开销，不参与运行时胜负。

## 覆盖的工作流

- 就绪任务、yield、spawn/join，以及普通线程上的 `block_on` 入口。
- 同 worker 内交接、跨 runtime 交接和外部通知。
- 无争用与有争用下的 mutex、semaphore、condition_variable、latch、barrier。
- MPSC 的立即完成、背压和多生产者传递路径。
- join、join_all、scope、select 的启动、汇合和排空。

## 和 Tokio 对照时要注意什么

Tokio 侧没有逐一对应的类型。条件变量和 latch 的对照用 Notify 加计数器拼出来，scope 用 JoinSet 组合。它们验证的是成功路径上的工作流，接口语义并不完全相同。Rust 的 ready future 可能被内联，C++ 的惰性 task 通常拥有独立协程帧，这部分差异会体现在分配和切换开销里。

## 帧分配与其他程序

`current_thread_benchmark.cpp` 对照调用线程直接驱动与一个后台 worker 驱动的差别。`frame_allocation_benchmark.cpp` 通过独立翻译单元里的工厂创建小帧、512 字节帧和 4 KiB 帧，并测量突发分配。缓存版与全局 `new` 版是两个独立 target，都关闭 LTO：

```sh
./target/cmake-build-linux-main-release/benchmark/faio_frame_allocation_benchmark 2000000
./target/cmake-build-linux-main-release/benchmark/faio_frame_allocation_new_benchmark 2000000
```

Bazel 下对应 `//benchmark:faio_frame_allocation_benchmark` 和 `//benchmark:faio_frame_allocation_new_benchmark`，xmake 下是 `faio_frame_allocation_benchmark` 和 `faio_frame_allocation_new_benchmark`。全局 `new` 版可以在 Linux 上把 `LD_PRELOAD` 指向 tcmalloc，观察分配器替换后的变化。

## 结果的可比性

worker 数量、CPU 集合、编译器和空闲策略都会改变结果。比较时记录实际配置，把预热和正式测量分开，多轮运行的数据和波动都保留。总览、程序清单和测量约定见[基准说明](../README.md)。
