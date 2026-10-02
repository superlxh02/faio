# Benchmark

**简体中文** | [English](README_EN.md)

## 1. 目录与统一入口

```text
benchmark/
  CMakeLists.txt
  README.md
  coro/
    faio_coro_benchmark.cpp      faio 的全部协程测试，一个 C++ 文件
    tokio-benchmark/             对应的 Tokio 项目，src/main.rs 为测试实现
    README.md                   更详细的协程协议和对照语义
  tcp/
    faio_tcp_benchmark.cpp
    asio_tcp_benchmark.cpp
    tokio-benchmark/
  result/
    coro/                       完成的正式协程数据
    tcp/                        后续运行 TCP 脚本时生成
```

| Suite | 实现 | 固定轮数 | 入口 |
|---|---|---:|---|
| coro | faio / Tokio，43 个场景，每轮 batch 与 sampled 两遍 | 10 | `python3 scripts/compare_coro_benchmarks.py` |
| tcp | faio / standalone Asio / Tokio | 3 | `python3 scripts/compare_tcp_benchmarks.py` |

`benchmark_report.py` 是共用统计/绘图模块，`network_benchmark.py` 是 TCP 运行器，两者位于 `scripts`，不是额外测试入口。C++ 产物位于 `build/benchmark-o2/benchmark`，Rust 产物位于 `build/benchmark-rust/<coro|tcp>`。

旧的独立 `coroutine_stress.cpp`、`coroutine_primitives.cpp`、`coroutine_scheduler.cpp` 与旧实验脚本已归档；正式协程对照只有 `coro` 中的一个 C++ 文件和一个 Tokio 项目。

## 2. 怎么规划和编写这些测试

### 2.1 先确定问题，再确定操作单位

测试按三个层次编写：

1. **基础路径**：直接 await 子任务、yield、无争用的同步原语，观察协程封装及立即完成路径的成本。
2. **竞争与并发工作流**：锁竞争、任务组、跨线程通知、多生产者 MPSC，观察入队、背压、唤醒、任务生命周期与结果收集的实际成本。
3. **网络端到端路径**：真实客户端请求，包含协议解析、应用响应、I/O 和调度，观察服务整体吞吐与请求延迟。

每个场景先定义“一次操作”是什么：一个 yield、一组 32 条任务、一条消息、一次双向交接或一个 HTTP 请求。不同单位的 ns/op、ops/sec 不直接横向排列成整体优劣排名。

### 2.2 两边实现同一工作流

- worker 数、参与者数、消息尺寸、队列容量与实际操作数一致。MPSC pipeline 两边均为 16 字节消息；网络服务正文统一为 16 字节。
- 不只比较接口名字：faio join 会启动独立分支，因此 Tokio 对照先 spawn 再等待句柄；select 对照也排空两个已提交分支。
- Tokio 没有原生条件变量/闩，分别用 Mutex+Notify、计数器+Notify 构造相同成功工作流，明确标为组合参考。
- worker 内执行的工作不放到 Rust host block_on 线程上直接执行。跨线程 RTT 用两个单 worker runtime 确保两端线程不同。
- 程序校验消息/任务结果、完成数量，脚本检查场景清单、操作数与数值有效性。错误/超时不参与不完整均值。

### 2.3 把计时与辅助工作分开

- runtime 创建/关闭、主要样本数组分配、统计和文件输出放在对应计时区间之外；具体场景中必需的任务组/队列操作依定义计时。
- 协程每个进程先完整预热；网络每个服务先运行一次预热负载。预热数据保存或丢弃，但都不参与均值。
- **batch** 关闭逐操作读时钟，仅用整段时间除以次数，观察摊销成本；保留循环和校验辅助操作，不声称是理论最小指令成本。
- **sampled** 每次读时钟并记录完整延迟，输出 P50/P90/P99/P99.9/max；单次延迟与并发墙钟摊销成本不是同一个概念。
- 单独保留时钟校准场景，不从纳秒级结果中强行相减。时钟离散量化可能令样本 P50 为 0，应结合 batch 数据解释。

### 2.4 多轮采集，再汇总与绘图

两边串行测量并轮换先后顺序；两个 suite 共用进程锁。所有测量完成后才运行 Python 压缩/绘图，减少图表生成对 CPU 的干扰。

结果采用各轮**算术均值**，同时保存样本标准差（ddof=1）、CV%、最小值、最大值、极差和中位数。P99 的总结果是“每轮 P99 的均值”，不是合并全部样本后的 P99。每轮数据与全部原始协程延迟均可回查，不删除离群值。

## 3. 环境、构建与运行

Linux，GCC、CMake、liburing、spdlog、standalone Asio、GoogleTest、rustc/cargo、wrk，以及 Python pandas/numpy/matplotlib。

C++ 固定 GCC `-O2 -DNDEBUG`，关闭 LTO，独立使用 `build/benchmark-o2`；脚本检查编译器实际为 GNU。Rust 使用 rustc，两个项目均为 `opt-level=2`、`lto=false`、`codegen-units=1`，依赖由 Cargo.lock 固定。

```bash
# Linux 上运行协程完整测试
python3 scripts/compare_coro_benchmarks.py
# 使用现有 Docker 环境，允许 io_uring 系统调用
docker run --rm --security-opt seccomp=unconfined \
  -v "$PWD:/workspace/faio" -w /workspace/faio \
  faio:dev python3 scripts/compare_coro_benchmarks.py
```

协程默认 count=100000，绑到 CPU 集合 `0-5`；可用 `--count` 和 `--cpus` 指定本机负载与允许的 CPU。单条操作大多执行 count 次；交接/通知/block_on 使用 min(count,10000)，32 任务组执行 floor(count/32) 次。CSV 的 `operations` 保存真实次数。

```bash
# 网络脚本保留，按需运行，不会被协程入口连带启动
python3 scripts/compare_tcp_benchmarks.py
# 调整负载不会改变固定三轮的统计规则
python3 scripts/compare_tcp_benchmarks.py --connections 512 --duration 20s --server-workers 4
```

当前已完成协程 10 轮；TCP 测试已停止。本次没有新协议下完成的网络汇总。

## 4. 协程所有测试项

下表逐项列出程序实际输出的 **43 个场景**。`w` 表示 worker 数，`p` 表示生产者/参与者，`c` 表示容量，`k` 表示许可数。“开销”指 batch 整段时间/次数，“延迟”指 sampled 单条时间戳区间。

### 基础与计时参考

| 测试项 | 配置 | 操作单位 | 测什么 / 怎样理解 |
|---|---|---|---|
| `timer_calibration` | host | 一次调用/完整操作 | 连续读时钟及写样本的参考成本；不作为运行时性能胜负指标。 |
| `task_await_ready` | 1 worker | 一次调用/完整操作 | await 返回 1 的子任务到获得结果；C++ 协程帧与可内联 Rust future 属于语言模型参考。 |

### 协程切换与唤醒

| 测试项 | 配置 | 操作单位 | 测什么 / 怎样理解 |
|---|---|---|---|
| `yield_1` | 1 worker | 一次调用/完整操作 | 当前任务 yield 到自身恢复；1 worker 保证同线程，4 worker 不保证迁移，另外记录迁移次数。 |
| `handoff_rtt_w1` | 1 worker | 一次往返 | 两条任务用请求/回复信号量交替；请求释放到回复获取的双向 RTT。w1 保证同线程，w4 不保证跨线程。 |
| `yield_4` | 4 worker | 一次调用/完整操作 | 当前任务 yield 到自身恢复；1 worker 保证同线程，4 worker 不保证迁移，另外记录迁移次数。 |
| `handoff_rtt_w4` | 4 worker | 一次往返 | 两条任务用请求/回复信号量交替；请求释放到回复获取的双向 RTT。w1 保证同线程，w4 不保证跨线程。 |
| `cross_runtime_rtt` | 2 runtime，各 1 worker | 一次往返 | 两个独立单 worker runtime，保证两端线程不同；两次信号量交接 RTT，不当作单向延迟。 |
| `external_notification_registered` | 1 worker | 一次通知 | 外部助手确认等待节点已登记后释放许可，测释放到协程恢复；助手忙轮询，排除提前已有许可的快路径。 |

### 同步竞争

| 测试项 | 配置 | 操作单位 | 测什么 / 怎样理解 |
|---|---|---|---|
| `mutex_contention_p4_w1` | 1 worker, 4 参与者 | 一次调用/完整操作 | 4 条任务竞争同一锁，每次加锁后持锁 yield 再解锁；延迟包含锁等待与切换。 |
| `semaphore_contention_k2_p4_w1` | 1 worker, 4 参与者, 2 permit | 一次调用/完整操作 | 4 条任务竞争 2 个许可，持许可 yield 再释放；观察背压等待与调度。 |
| `barrier_p4_w1` | 1 worker, 4 参与者 | 一次屏障到达 | 4 条任务重复参加四方屏障，各执行 count/4 次；一次操作为一次参与者到达并恢复。 |
| `mutex_contention_p4_w4` | 4 worker, 4 参与者 | 一次调用/完整操作 | 4 条任务竞争同一锁，每次加锁后持锁 yield 再解锁；延迟包含锁等待与切换。 |
| `semaphore_contention_k2_p4_w4` | 4 worker, 4 参与者, 2 permit | 一次调用/完整操作 | 4 条任务竞争 2 个许可，持许可 yield 再释放；观察背压等待与调度。 |
| `barrier_p4_w4` | 4 worker, 4 参与者 | 一次屏障到达 | 4 条任务重复参加四方屏障，各执行 count/4 次；一次操作为一次参与者到达并恢复。 |

### 同步组合参考

| 测试项 | 配置 | 操作单位 | 测什么 / 怎样理解 |
|---|---|---|---|
| `cv_roundtrip_w1` | 1 worker | 一次往返 | 谓词握手：持锁修改状态、通知、等待回复并重新持锁；Tokio Mutex+Notify 组合参考。 |
| `latch_fanin_32_w1` | 1 worker | 一组 32 个任务 | 创建计数 32 的闩，32 条任务 count_down，等待打开并排空句柄；Tokio AtomicUsize+Notify 组合参考。 |
| `cv_roundtrip_w4` | 4 worker | 一次往返 | 谓词握手：持锁修改状态、通知、等待回复并重新持锁；Tokio Mutex+Notify 组合参考。 |
| `latch_fanin_32_w4` | 4 worker | 一组 32 个任务 | 创建计数 32 的闩，32 条任务 count_down，等待打开并排空句柄；Tokio AtomicUsize+Notify 组合参考。 |
| `latch_ready` | 1 worker | 一次调用/完整操作 | 等待已归零的闩；Tokio 计数器检查组合参考，无原生 latch。 |

### 并发接口

| 测试项 | 配置 | 操作单位 | 测什么 / 怎样理解 |
|---|---|---|---|
| `spawn_join_1` | 1 worker | 一次调用/完整操作 | spawn 一条返回 1 的任务，await JoinHandle 到获得结果；包含任务创建、入队与结果传递。 |
| `spawn_join_4` | 4 worker | 一次调用/完整操作 | spawn 一条返回 1 的任务，await JoinHandle 到获得结果；包含任务创建、入队与结果传递。 |
| `join_all_32` | 1 worker | 一组 32 个任务 | 创建并启动 32 条任务，按顺序收集所有结果并校验和 32；一次操作是一组。 |
| `scope_32` | 1 worker | 一组 32 个任务 | 创建 scope，提交 32 条任务并等待全部结束；Tokio JoinSet 成功完成流程参考，不声称取消语义相同。 |
| `join_ready_2` | 1 worker | 一组 2 个任务 | 启动两条独立任务并取得两条结果；Tokio 先 spawn 再 join!，一次操作是一组。 |
| `select_spawn_drain_2` | 1 worker | 一组 2 个任务 | 提交两个立即完成任务、选择赢家并排空另一分支；Tokio 在 JoinHandle 间 select 后 await 另一句柄。 |

### MPSC 队列

| 测试项 | 配置 | 操作单位 | 测什么 / 怎样理解 |
|---|---|---|---|
| `mpsc_p1_w1_c64` | 1 worker, 1 producer, 64 容量 | 一条消息 | 一个消费者，按名字配置生产者/worker/容量；16 字节消息，发送前到接收的延迟含背压和排队；总数 count 并校验和。 |
| `mpsc_p4_w1_c64` | 1 worker, 4 producer, 64 容量 | 一条消息 | 一个消费者，按名字配置生产者/worker/容量；16 字节消息，发送前到接收的延迟含背压和排队；总数 count 并校验和。 |
| `mpsc_p1_w1_c1024` | 1 worker, 1 producer, 1024 容量 | 一条消息 | 一个消费者，按名字配置生产者/worker/容量；16 字节消息，发送前到接收的延迟含背压和排队；总数 count 并校验和。 |
| `mpsc_p4_w1_c1024` | 1 worker, 4 producer, 1024 容量 | 一条消息 | 一个消费者，按名字配置生产者/worker/容量；16 字节消息，发送前到接收的延迟含背压和排队；总数 count 并校验和。 |
| `mpsc_p1_w4_c64` | 4 worker, 1 producer, 64 容量 | 一条消息 | 一个消费者，按名字配置生产者/worker/容量；16 字节消息，发送前到接收的延迟含背压和排队；总数 count 并校验和。 |
| `mpsc_p4_w4_c64` | 4 worker, 4 producer, 64 容量 | 一条消息 | 一个消费者，按名字配置生产者/worker/容量；16 字节消息，发送前到接收的延迟含背压和排队；总数 count 并校验和。 |
| `mpsc_p1_w4_c1024` | 4 worker, 1 producer, 1024 容量 | 一条消息 | 一个消费者，按名字配置生产者/worker/容量；16 字节消息，发送前到接收的延迟含背压和排队；总数 count 并校验和。 |
| `mpsc_p4_w4_c1024` | 4 worker, 4 producer, 1024 容量 | 一条消息 | 一个消费者，按名字配置生产者/worker/容量；16 字节消息，发送前到接收的延迟含背压和排队；总数 count 并校验和。 |
| `mpsc_ready_64` | 1 worker, 64 容量 | 一对发送/接收 | 同一任务在容量 64 的队列 async send+recv；8 字节负载，一次操作是一对，无协程间切换。 |
| `mpsc_try_64` | 1 worker, 64 容量 | 一对发送/接收 | 同一任务容量 64，try_send+try_recv；8 字节负载，一次操作是一对，测非挂起路径。 |

### 任务提交

| 测试项 | 配置 | 操作单位 | 测什么 / 怎样理解 |
|---|---|---|---|
| `external_burst_1` | 1 worker | 一个任务 | host 批量 spawn_detached，测提交前到任务开始的排队延迟；整段到全部任务完成信号到达。 |
| `internal_burst_1` | 1 worker | 一个任务 | worker 内批量 spawn_detached，测提交前到任务开始的排队延迟；观察本地队列与溢出路径。 |
| `external_burst_4` | 4 worker | 一个任务 | host 批量 spawn_detached，测提交前到任务开始的排队延迟；整段到全部任务完成信号到达。 |
| `internal_burst_4` | 4 worker | 一个任务 | worker 内批量 spawn_detached，测提交前到任务开始的排队延迟；观察本地队列与溢出路径。 |
| `block_on_entry` | 1 worker | 一次入口往返 | host 提交任务到 worker，等待返回 1；Tokio 使用 block_on(spawn(task))，测 host→worker→host。 |

### 同步立即完成路径

| 测试项 | 配置 | 操作单位 | 测什么 / 怎样理解 |
|---|---|---|---|
| `semaphore_ready` | 1 worker | 一次调用/完整操作 | 已有一个许可，单任务 acquire+release 的无争用路径。 |
| `mutex_ready` | 1 worker | 一次调用/完整操作 | 单任务 lock+unlock 的无争用路径；Tokio guard 析构释放锁。 |
| `barrier_ready_1` | 1 worker | 一次调用/完整操作 | 单参与者屏障 arrive_and_wait，测立即完成路径。 |

### 协程指标含义

| CSV 字段 | 单位 | 含义 |
|---|---|---|
| `batch_ns_per_op` | ns/该场景单位 | 关闭逐次时钟的摊销墙钟成本，越低越好 |
| `sampled_ns_per_op` | ns/该场景单位 | 包含逐次采样的摊销成本，不冒充净切换成本 |
| `ops_per_sec` | 该场景单位/秒 | 1e9 / batch_ns_per_op；分组场景是组/秒 |
| `p50_ns`, `p90_ns`, `p99_ns`, `p999_ns` | ns | 该场景操作/消息延迟的 50%、90%、99%、99.9% 分位数 |
| `max_ns` | ns | 本轮样本最大延迟，包括系统抢占造成的尾部 |
| `migrations`, `batch_migrations` | 次 | yield 前后 worker/线程变化次数；分别来自 sampled 与 batch；不是 CPU 核迁移计数 |
| `operations` | 次 | 本轮实际操作/消息/组数，与操作单位一起解释 |
| `workers`, `producers`, `capacity`, `unit`, `comparison` | 配置/标签 | 描述工作量与语义可比性；不是需要比较快慢的指标 |

更详细的生命周期、背压、采样协议与 Tokio 组合差异见 [coro/README.md](coro/README.md)。目前 select 只测立即完成分支；取消慢路径/异常传播/公平性属于另外的语义验证，没有伪称已覆盖这些性能负载。

## 5. TCP 所有测试项与实现

| 实现 | 测试代码 | 实现路径 |
|---|---|---|
| faio | `tcp/faio_tcp_benchmark.cpp` | TcpListener 接入，每连接一个 faio task，异步 read / write_all |
| Asio | `tcp/asio_tcp_benchmark.cpp` | co_spawn，每连接一个 awaitable，async_read_some / async_write |
| Tokio | `tcp/tokio-benchmark/src/main.rs` | tokio::spawn，TcpStream read / write_all |

三者执行相同的 **`keep_alive_get_index`** 负载：本机 wrk 发 HTTP/1.1 GET /index，经 TCP 按 `\r\n\r\n` 拆帧，返回固定响应，继续读取同一连接上的下一请求。正文 `hello benchmark\n` 为 16 字节，Content-Length 一致。它用于测 TCP I/O+协程处理固定请求/响应的完整路径，不是完整 HTTP 实现比较。

程序编写顺序：接入循环 → 启动独立连接任务 → 读入缓冲 → 查找请求边界 → 完整写回 → 保留后续请求。运行器先检查服务状态码/正文，再预热、采集 wrk 指标、关闭服务。下方 11 个指标在每个实现、每轮都输出。

### TCP 完整指标表

| 测试项 / CSV 字段 | 单位 | 含义与比较方向 |
|---|---|---|
| `requests_per_sec` | 请求/秒 | wrk 完成请求吞吐；越高越好，需一起检查错误数 |
| `latency_avg_ms` | ms | wrk 报告的平均请求延迟，越低越好 |
| `latency_p50_ms` | ms | 一半请求不超过的延迟，观察常见延迟 |
| `latency_p90_ms` | ms | 90% 分位数，观察较慢请求 |
| `latency_p99_ms` | ms | 99% 分位数，观察尾延迟 |
| `timeout_count` | 次 | wrk 超时数量，越少越好 |
| `connect_errors` | 次 | 建连失败数量，不能视作完成了请求 |
| `read_errors` | 次 | 读取响应失败数量 |
| `write_errors` | 次 | 发送请求失败数量 |
| `non_success_count` | 次 | wrk 报告的非 2xx/3xx 响应数；此负载正常应为 200 |
| `transfer_bytes_per_sec` | 字节/秒 | wrk Transfer/sec 统一换算为字节；含 HTTP 头，不是纯正文吞吐 |

默认每个实现每轮先 `wrk -t4 -c5000 -d3s` 预热，再 `wrk --latency -t4 -c5000 -d60s` 采集，完整 3 轮。三种实现依轮次轮换顺序，各在首/中/尾位置测一次。

wrk 是闭环负载，同机客户端/服务竞争 CPU。完整 stdout 与文本直方图都保存；wrk 没有输出逐请求时间戳，故这里不生成伪造的逐请求 CSV 或 P99.9。三轮波动只能作为粗略稳定性观察，不冒充精确置信区间。

## 6. 所有结果与图表

每次输出使用新目录 `benchmark/result/<suite>/<UTC 时间>/`，成功后更新 `latest.txt`。被叫停/失败的运行不会形成完整均值，也不会更新 latest。

| 文件 / 图表 | 内容 |
|---|---|
| `round_XX/comparison.csv` | 本轮所有实现、场景、指标 |
| `round_XX/<category>_bar.png` | 本轮各类别所有数值性能指标柱状图 |
| `round_XX/<category>_line.png` | 本轮按场景排列的比较折线，不代表时间演进 |
| `round_XX/samples/<faio|tokio>/*.csv.gz` | 协程的全部逐次原始延迟，保持采集顺序 |
| `round_XX/latency_distribution_*.png` | 协程各场景经验分位数曲线，包含尾部 |
| `round_XX/<implementation>_{warmup,measured}.txt` | 网络的预热与正式 wrk 原始输出 |
| `round_XX/<implementation>_server.log` | 网络服务输出，供检查错误 |
| `all_rounds.csv` | 10/3 轮全部数值，不做离群值裁剪 |
| `summary.csv`, `summary.md` | 所有指标各轮的算术均值及完整表 |
| `statistics.csv` | 每项 mean/stddev/CV/min/max/range/median |
| `mean/<category>_{bar,line}.png` | 总均值柱状图与比较折线 |
| `<category>_mean_stddev.png` | 总均值和样本标准差误差线 |
| `<category>_round_variation.png` | 所有指标跨 10/3 轮的波动，按各自均值归一化 |
| `ratios.csv` | 协程 faio/Tokio 的均值之比，附可比性标签；开销/延迟小于 1 表示 faio 较小 |
| `process_usage.csv` | 协程 40 次完整程序的 user/system/wall 耗时，含预热和输出，不是逐场景 CPU 用量 |
| `metadata.json`, `commands.json`, `compile_commands.json` | 真实环境、顺序、参数、编译选项 |
| `source_snapshot.tar.gz` | 测试时库/benchmark 源码、脚本和依赖锁，便于复现未提交的工作区状态 |

图表按类别及指标拆分，各指标使用自己的坐标轴；跨度大时注明 log scale。PNG 显示可视化，CSV 保留精确数值。CV 很高时先分析波动，不能仅凭一个均值宣称稳定提升。

## 7. 清理与历史数据

协程新协议的完整 10 轮结果保留在 `result/coro`。旧独立程序、旧实验脚本/原始数据、旧单轮网络结果、被叫停的 TCP 运行已归档到工作区外：

```text
/Users/lxh/workspace/cpp/faio_backups/benchmark_cleanup_20261002_110446
```

备份按原相对路径保存，可用于追溯旧文档中的测量。旧 5/7/9/11 轮中位数实验不加入正式 10 轮算术均值。
