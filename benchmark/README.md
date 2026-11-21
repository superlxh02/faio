# 性能基准

**简体中文** | [English](README_EN.md)

本目录提供协程、文件、CPU 与 TCP 基准，以及由 Cargo.lock 锁定版本的 Tokio 对照。各程序输出 CSV；基准与单元测试分开运行。

## 构建

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release -j4
```

依赖为 spdlog、standalone Asio 及所选平台库。Tokio 对照另需 Rust/Cargo，依赖版本由各项目的 Cargo.lock 固定。主线预设使用 Clang 23。比较不同运行时时，应显式固定优化级别、worker 数量与 CPU 集合。

| 程序 | 内容 |
| --- | --- |
| `faio_coro_benchmark` | 任务、切换、同步原语和并发工作流 |
| `faio_current_thread_benchmark` | 单线程运行时与阻塞池 |
| `faio_frame_allocation_benchmark` | 缓存协程帧分配 |
| `faio_frame_allocation_new_benchmark` | 全局 new/delete 帧分配 |
| `faio_mpsc_ready_queue_benchmark` | 多生产者就绪队列 |
| `faio_io_cpu_benchmark` | 普通文件与 CPU 任务 |
| `faio_blocking_pool_benchmark` | 固定与弹性用户阻塞池 |
| `faio_tcp_benchmark`、`asio_tcp_benchmark` | TCP 服务对照 |
| `tcp_echo_load` | 持久连接 echo 客户端，输出 QPS 与时延 |
| `tcp_connect_load` | 每连接一次请求的短连接客户端 |

CMake 按平台选择可构建的程序。

## 协程与帧分配

```sh
./build/linux-main-release/benchmark/faio_coro_benchmark 100000 batch
./build/linux-main-release/benchmark/faio_coro_benchmark 100000 sampled
./build/linux-main-release/benchmark/faio_frame_allocation_benchmark 2000000
./build/linux-main-release/benchmark/faio_frame_allocation_new_benchmark 2000000
cargo build --manifest-path benchmark/coro/tokio-benchmark/Cargo.toml --release --locked
```

`batch` 用总耗时除以实际操作数，`sampled` 逐项计时并输出分位数。两种口径要分开解读；协程场景的工作单位见[协程基准](coro/README.md)。

在 Linux 上，可为全局 new 程序设置 `LD_PRELOAD` 指向已安装的 tcmalloc 共享库，以对比系统分配器。两种帧基准使用独立的负载翻译单元并关闭 LTO，防止分配被编译器消除。

## 文件与 CPU

```sh
mkdir -p build/perf-files
./build/linux-main-release/benchmark/faio_io_cpu_benchmark build/perf-files 4096 4 all
cargo build --manifest-path benchmark/io/tokio-benchmark/Cargo.toml --release --locked
```

最后一个参数可取 `io`、`cpu` 或 `all`。文件路径由调用方提供，程序创建并清理自己的测试文件。热页缓存吞吐与冷盘吞吐、耐久写入是不同问题。

## 用户阻塞池

```sh
./build/linux-main-release/benchmark/faio_blocking_pool_benchmark fixed 64
./build/linux-main-release/benchmark/faio_blocking_pool_benchmark elastic 64
```

分别测量无任务的阻塞池和 current-thread runtime 的构造与销毁耗时。调度延迟从提交任务前计到闭包开始执行。直接提交到池和通过 `spawn_blocking` 提交均使用 2 ms 的稀疏间隔。冷突发间隔 90 ms，弹性保活时间设为 50 ms，以便在一次测量中采集多个回收周期。每个冷突发连续提交 16 个任务，每个任务模拟 1 ms 的阻塞。热吞吐测量最小任务和含 20000 轮整数计算的任务。

CSV 的 `blocking_threads` 只统计用户阻塞池的工作线程。冷突发行分别记录突发前和提交整批任务后的最大快照。

## TCP

分别在两个终端启动服务与客户端：

```sh
./build/linux-main-release/benchmark/faio_tcp_benchmark 127.0.0.1 9000 4 echo
```

```sh
./build/linux-main-release/benchmark/tcp_echo_load 127.0.0.1 9000 4 64 64 10 1
```

客户端参数依次为地址、端口、客户端线程数、连接数、payload 字节数、测量秒数和预热秒数。输出包含校验成功请求的 QPS、平均时延、P99 及错误数。

Tokio 服务：

```sh
cargo build --manifest-path benchmark/tcp/tokio-benchmark/Cargo.toml --release --locked
```

## 测量约定

比较时固定协议、负载、worker 数量、CPU 亲和性、预热和优化参数，各实现串行运行。检测器、采样器、编译和绘图放在计时之外。每次测量使用独立进程，保存每轮 CSV、源码与二进制摘要、均值、样本标准差和 CV；离群值保留，不剔除。

QPS 统计窗口内完成的请求；时延样本还包含窗口内发起、窗口后完成的慢请求。各轮 P99 的均值不等于合并所有样本后的 P99。出现错误或超时时，应记录失败并说明不完整数据的边界。
