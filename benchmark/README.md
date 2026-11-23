# 性能基准

**简体中文** | [English](README_EN.md)

这些程序测量协程调度、协程帧分配、文件与 CPU 任务、用户阻塞池以及 TCP 服务。除两个 TCP 服务端程序外，结果都按 CSV 打到标准输出，重定向后可以直接接脚本汇总。基准不进入测试套件，需要你手动运行，也没有性能回归门槛。

C++ 部分需要支持 C++23 的编译器、spdlog 和平台线程库，CMake 下的 `asio_tcp_benchmark` 另需 standalone Asio。与 Tokio 对照的程序用 Rust 写，依赖由各自的 `Cargo.lock` 锁定。比较不同运行时时，先固定优化级别、worker 数量和 CPU 集合，否则数字没有意义。

## 构建与运行

三套构建系统都把产物写在 `target/` 的不同子目录里，互不覆盖。CMake 预设默认打开 `FAIO_BUILD_BENCHMARKS`，是最省事的入口：

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release -j4
```

产物在 `target/cmake-build-linux-main-release/benchmark/`。macOS 把预设换成 `mac-main-release` 即可。Linux 主线预设使用 Clang 23。

仓库把 Bazel 锁在 9.2.0。基准目标都带 `manual` 标签，`//...` 不会构建它们，得按名字指定：

```sh
bazel build --config=faio_headers //benchmark:faio_coro_benchmark
bazel run --config=faio_headers //benchmark:faio_coro_benchmark -- 100000 batch
```

产物链接在 `target/bazel/bin/benchmark/`。在 Linux 开发容器里跑文件和网络基准时，如果容器不允许 io_uring 系统调用，加上 `--config=faio_epoll`。Asio 对照由 Bzlmod 拉取固定版本，不用额外准备头文件。

xmake 需要 3.0.1 或更新版本。基准默认关闭，而且只在头文件目标开启时注册，所以配置时要显式打开：

```sh
xmake f -c -y -m release --faio_benchmarks=y
xmake build faio_coro_benchmark
xmake run faio_coro_benchmark 100000 batch
```

`xmake run` 会把 target 名后面的参数原样交给程序。需要 `asio_tcp_benchmark` 时，先把 `FAIO_ASIO_INCLUDE_DIR` 设为 `asio.hpp` 所在目录再配置。xmake 选择工具链的方式与构建库时相同，默认编译器不支持 C++23 时按[构建配置](../docs/getting-started.md#xmake-构建)传入 `--toolchain=clang` 和对应的 `--cxx`、`--ld` 路径。

基准始终按头文件形态编译。Bazel 目标显式关闭模块特性，xmake 只在 `--faio_headers` 开启时注册它们，所以你不需要为跑基准启用具名模块。

各程序的分工如下。三套构建都会按平台裁剪目标：Windows 上没有 `faio_current_thread_benchmark` 和 `tcp_connect_load`。CMake 下 `asio_tcp_benchmark` 另需找到 Asio。

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

三个 Tokio 对照项目在 `benchmark/*/tokio-benchmark/` 下。从仓库根目录或这些子目录运行 Cargo，程序都生成在 `target/cargo/release/`。

## 协程与帧分配

先在 CMake 下运行：

```sh
./target/cmake-build-linux-main-release/benchmark/faio_coro_benchmark 100000 batch
./target/cmake-build-linux-main-release/benchmark/faio_coro_benchmark 100000 sampled
./target/cmake-build-linux-main-release/benchmark/faio_frame_allocation_benchmark 2000000
./target/cmake-build-linux-main-release/benchmark/faio_frame_allocation_new_benchmark 2000000
cargo build --manifest-path benchmark/coro/tokio-benchmark/Cargo.toml --release --locked
```

`batch` 用总耗时除以实际操作数，`sampled` 逐项读时钟并给出分位数。两种口径混在一起排不出可信的结论，要分开解读。协程场景的工作单位见[协程基准](coro/README.md)。改用 Bazel 或 xmake 时参数不变，把上面的可执行文件换成 `bazel run --config=faio_headers //benchmark:faio_coro_benchmark -- 100000 batch` 或 `xmake run faio_coro_benchmark 100000 batch`。

两个帧分配程序分别使用缓存和全局 `new`。在 Linux 上可以把 `LD_PRELOAD` 指向已安装的 tcmalloc 共享库，与系统分配器对照。它们使用独立的负载翻译单元并关闭 LTO，避免分配被编译器优化掉。

## 文件与 CPU

```sh
mkdir -p target/perf-files
./target/cmake-build-linux-main-release/benchmark/faio_io_cpu_benchmark target/perf-files 4096 4 all
cargo build --manifest-path benchmark/io/tokio-benchmark/Cargo.toml --release --locked
```

最后一个参数取 `io`、`cpu` 或 `all`，`4096` 是 4 KiB 操作数。测试目录必须已经存在，程序在里面创建并清理自己的测试文件。热页缓存下的吞吐、冷盘读取和耐久写入是三个不同的问题，报告时写清测的是哪一种。

## 用户阻塞池

```sh
./target/cmake-build-linux-main-release/benchmark/faio_blocking_pool_benchmark fixed 64
./target/cmake-build-linux-main-release/benchmark/faio_blocking_pool_benchmark elastic 64
```

两种策略分别测无任务阻塞池和 current-thread runtime 的构造与销毁开销。调度延迟在提交前打时间戳，到闭包开始执行时截止。直接提交到池和经 `spawn_blocking` 提交都使用 2 ms 的稀疏间隔。冷突发间隔 90 ms，弹性池的保活时间设成 50 ms，这样一次测量能覆盖多个回收周期。每个冷突发连续提交 16 个任务，每个任务阻塞 1 ms。热吞吐则测最小任务和包含 20000 轮整数运算的任务。

CSV 里的 `blocking_threads` 只统计用户阻塞池的工作线程。冷突发行分别记录突发前和整批任务提交后的最大线程数。

## TCP

服务端和客户端各开一个终端：

```sh
./target/cmake-build-linux-main-release/benchmark/faio_tcp_benchmark 127.0.0.1 9000 4 echo
```

```sh
./target/cmake-build-linux-main-release/benchmark/tcp_echo_load 127.0.0.1 9000 4 64 64 10 1
```

客户端参数依次是地址、端口、客户端线程数、连接数、payload 字节数、测量秒数和预热秒数。输出给出校验通过请求的 QPS、平均时延、P99 和错误数。短连接客户端 `tcp_connect_load` 接收地址、端口、并发连接数、payload 字节数、测量秒数和预热秒数。

Tokio 服务：

```sh
cargo build --manifest-path benchmark/tcp/tokio-benchmark/Cargo.toml --release --locked
```

## 测量约定

比较时固定协议、负载、worker 数量、CPU 亲和性、预热和优化参数，各实现串行运行。检测器、采样器、编译和绘图都放在计时之外。每次测量使用独立进程，保存每轮 CSV、源码与二进制摘要、均值、样本标准差和 CV。离群值保留，不剔除。

QPS 统计窗口内完成的请求；时延样本还包含窗口内发起、窗口后才完成的慢请求。各轮 P99 的均值不等于合并所有样本后的 P99。出现错误或超时，记录失败原因，并说明数据不完整覆盖到哪一段。
