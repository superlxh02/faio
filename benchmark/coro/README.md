# 协程 benchmark 协议

**简体中文** | [English](README_EN.md)

本目录只保留一个 faio C++ 测试程序和一个 Tokio Rust 项目。入口为 `scripts/compare_coro_benchmarks.py`，结果写入 `benchmark/result/coro/<UTC 时间>/`。

## 一、运行方式与固定规则

```bash
python3 scripts/compare_coro_benchmarks.py
```

默认每轮每个实现运行两遍，每遍包含 43 个场景，完整执行 **10 轮**。默认 `count=100000`，两边绑到同一 CPU 集合 `0-5`，可用 `--cpus` 指定本机允许的集合。`--count` 可以改变负载；它必须不小于 1024 且能被 4 整除。轮数固定，避免一次试跑的结果被当作正式均值。

Linux Docker 环境（本仓库依赖 io_uring）：

```bash
docker run --rm --security-opt seccomp=unconfined \
  -v "$PWD:/workspace/faio" -w /workspace/faio \
  faio:dev python3 scripts/compare_coro_benchmarks.py
```

- C++：CMake 独立目录 `build/benchmark-o2`，检查实际编译器为 GNU GCC，明确指定 `-O2 -DNDEBUG`，关闭 LTO。
- Rust：锁定 Tokio **1.49.0**，`cargo build --release --locked`，`opt-level=2`、`lto=false`、`codegen-units=1`。Rust 使用 rustc；“GCC -O2”只适用于 C++。
- 两边都使用多线程 runtime，即使 worker 数为 1。Tokio 开启 I/O/time driver，faio 正常创建 I/O 引擎；runtime 初始化和关闭不计入场景耗时。
- 每个测试进程先以 `min(count,1024)` 完整预热一遍，不输出预热数据。worker 恢复还会预热 1000 次 yield。不同场景不并行运行。
- 两个实现与两种计时模式的顺序按奇偶轮交替。TCP/HTTP 与协程测试共用进程锁，防止同一 checkout 的测试互相争抢 CPU。
- 测量时不启动 Python 绘图；全部轮次采集完成后才压缩原始样本并绘图。
- 超时、错误返回、校验和不正确、场景缺失或两边操作数不同会使本次测试失败，不用剩下的轮数补出均值。未完成运行不会更新 `latest.txt`。

## 二、两种计时模式

| 模式 | 输出 | 用途与边界 |
|---|---|---|
| batch | `batch_ns_per_op`、`ops_per_sec` | 关闭逐操作读时钟，整段墙钟时间除以操作数。仍保留循环、条件分支、结果写入与校验，不宣称是剥离一切辅助代码的理论指令成本 |
| sampled | `sampled_ns_per_op`、P50/P90/P99/P99.9/max | 每次操作读时钟并写样本；分位数展示操作延迟，整段时间含采样开销 |

`ns_per_op` 是摊销墙钟成本，并发操作的样本延迟可能显著大于它，不能混为同一指标。每个场景的计时边界如下表。吞吐量由 batch 模式 `1e9 / batch_ns_per_op` 得到，单位由 `scenarios.csv` 中的 `unit` 决定：一组 32 个任务的吞吐量是“组/秒”，不能当作单任务/秒。

延迟使用单调时钟。样本分位数采用排好序后的 `floor((N-1)*q)`，不做插值。原始样本按采集顺序写入 `.csv.gz`；统计和输出文件不计入场景计时。样本文件的写入仍会影响随后场景的缓存/温度，固定相同协议并轮换顺序可减小偏差，但不能消除它。

`timer_calibration` 是读时钟、循环和写样本的参考成本，**不从其他结果中强行相减**。虚拟机时钟可能出现约几十 ns 的离散步进，P50 为 0 并不代表操作零成本。应结合 batch 指标和原始分布判断。

## 三、场景矩阵与操作定义

`w1/w4` 表示 1/4 个 worker；`p` 表示生产者/参与者数；`c` 表示容量；`k` 表示许可数。全部 43 个场景也以机器可读形式存入每次输出的 `scenarios.csv`。

| 场景 | 组合/数量 | 每条样本的计时边界 | 说明 |
|---|---:|---|---|
| `timer_calibration` | 1 | 连续两次读时钟 | 时钟与采样参考 |
| `task_await_ready` | 1 | 调用子任务到取回 1 | faio 惰性 task 的对称转移 vs 可被内联的 Rust future，属于语言模型参考，不能作为调度器胜负依据 |
| `yield_1`, `yield_4` | 2 | yield 到当前任务恢复 | 单独一个可运行任务；4 worker **不保证**迁移，另报 `migrations` |
| `handoff_rtt_w1/w4` | 2 | 请求许可释放到回复许可获取 | 同一 runtime 的两条任务交替；w1 保证同线程两次交接，w4 可能在相同或不同线程执行 |
| `cross_runtime_rtt` | 1 | 请求释放到回复获取 | 两个独立单 worker runtime，保证两端 OS 线程不同；是两次交接 RTT，不把 RTT/2 伪称单向实测 P99 |
| `external_notification_registered` | 1 | 外部线程释放许可到已登记等待者恢复 | 等待者已登记才发布 armed 标志，排除提前已有 permit 的快路径；外部助手忙轮询确认，CPU 成本与普通闲置服务不同 |
| `spawn_join_1/4` | 2 | spawn 一个返回 1 的任务到 JoinHandle 获取结果 | 创建、提交、恢复、结果传递与句柄释放 |
| `join_ready_2` | 1 | join 两条任务到收集两条结果 | Rust 先 spawn 两条任务再 join!，对齐 faio 独立分支模型 |
| `join_all_32` | 1 | 构造 32 条任务到按顺序收集全部结果 | 每样本是一组，组数 `floor(count/32)`；两边校验结果和 32 |
| `scope_32` | 1 | 创建组、提交 32 条任务到全部排空 | faio scope vs Tokio JoinSet 的成功完成工作流；不主张两者异常/取消/借用语义相同 |
| `select_spawn_drain_2` | 1 | 提交两个立即完成任务、选择赢家、排空另一分支 | Rust select! 在两个 JoinHandle 间选择并 await 另一句柄，不能用丢弃普通 future 的 native select! 代替 faio 的协作取消与排空成本 |
| `external_burst_1/4` | 2 | host 提交前的时间戳到子任务开始执行 | faio spawn_detached；Rust 提交后丢弃 JoinHandle；整段计时到全部样本写入、完成信号到达 |
| `internal_burst_1/4` | 2 | worker 提交前到子任务开始执行 | worker 内批量 spawn_detached，不保留逐个结果句柄 |
| `block_on_entry` | 1 | host 提交、worker 执行、host 获得结果 | Rust block_on(spawn(task))；不是 host 上直接 poll 一个 Ready future。操作数最多 10000 |
| `semaphore_ready` | 1 | acquire 到 release | 一条任务、一个已有 permit、无争用 |
| `mutex_ready` | 1 | lock 到 unlock | 无争用；Rust guard 析构解锁 |
| `barrier_ready_1` | 1 | 一参与者 arrive_and_wait 完成 | 屏障立即完成路径 |
| `latch_ready` | 1 | 等待已打开 latch 完成 | Tokio 无原生 latch，使用计数器检查，属于组合参考 |
| `mutex_contention_p4_w1/w4` | 2 | 加锁、持锁 yield、解锁 | 4 条任务共用一把锁，刻意制造等待队列；总操作数 count |
| `semaphore_contention_k2_p4_w1/w4` | 2 | 获取许可、持许可 yield、释放 | 4 条任务、2 个 permit；不与无争用 acquire 指标混算 |
| `barrier_p4_w1/w4` | 2 | 每次到达四方屏障到恢复 | 每任务 count/4 次，同代参与者必须全部到齐；ns/op 是每次 arrival 摊销成本 |
| `cv_roundtrip_w1/w4` | 2 | 修改谓词、通知、等待回复谓词并重新持锁 | faio condition_variable vs Tokio Mutex+Notify 组合；登记通知后解锁、恢复后循环检查谓词，避免丢通知 |
| `latch_fanin_32_w1/w4` | 2 | 建立 latch、32 条任务 count_down、等待打开、排空句柄 | Tokio AtomicUsize+Notify 单等待者组合；每样本一组 32，成功完成语义一致，不等同原生类型 |
| `mpsc_ready_64`, `mpsc_try_64` | 2 | 一次 send+recv / try_send+try_recv | 同一任务容量 64，8 字节负载，没有等待队列；不是生产者消费者切换测试 |
| `mpsc_p{1,4}_w{1,4}_c{64,1024}` | 8 | 发送调用前时间戳到消费者获得消息 | 一个消费者，1/4 个生产者，1/4 worker，容量 64/1024；16 字节消息，两边校验所有 value 之和；延迟含背压、排队、唤醒 |

切换/条件变量 RTT、登记后外部通知、block_on 使用 `min(count,10000)`，其余多数逐次操作使用 count。32 任务分组使用 `floor(count/32)`，不足一组的余数不计入该场景。CSV 明确保存实际 `operations`。

### 可比性限制

1. C++ task 使用独立协程帧；Rust ready future 可能内联。语言差异是事实，不能为追求相同分配次数而改用另一种 Rust 用法。
2. Tokio 的 CV/latch 使用组合实现，`comparison` 分别标注 `tokio_notify_proxy`、`tokio_counter_notify_proxy`、`tokio_counter_proxy`；scope 标注 `joinset_proxy`。它们只比较表中成功工作流。
3. select 场景只测两个立即完成分支的成功选择和排空。挂起分支的取消、错误传播、嵌套 scope 取消属于语义验证范围，本轮性能矩阵不覆盖它们。
4. 所有并发原语测量其实际启动执行工作流。faio `join(...)` 普通调用构造惰性 task，本身没有执行并发任务；因此不把构造返回值当作完成了 join 的性能数据。`co_await join(...)` 与 `block_on(join(...))` 驱动相同惰性组合，后者入口开销由 `block_on_entry` 展示。
5. faio 默认空闲轮询 32 次；Tokio 使用自身默认调度/休眠策略。低唤醒延迟可能消耗更多 CPU。`process_usage.csv` 提供每次整个测试进程的 user/system/wall 耗时（含预热、初始化与文件输出），不能把它冒充某个单场景的 CPU 指标。
6. mpsc 保留时间戳而不是直接携带 Rust Instant，确保两边 sizeof(message)=16；batch 模式保留这个布局，但不读逐条时钟。Tokio 自身协作预算保持默认，faio 使用库当前默认，不人为关闭某一侧的让出机制。
7. Rust 任务跨线程安全借用受 Send/'static 约束，部分测试使用 Arc；faio 通过结构化等待保证引用有效。这类生命周期管理开销会进入完整工作流成本，不能据此推断底层单个队列操作的指令成本。
8. Docker 内核、共享宿主机、CPU 调度与计时量化都会影响尾延迟。这是当前 Linux ARM64 环境的观测结果，不外推成 x86 裸机结论。归档实际内核、编译器、CPU、affinity、构建命令与二进制哈希。

## 四、结果文件

```text
benchmark/result/coro/<timestamp>/
  metadata.json                 环境、配置、编译器和二进制校验值
  compile_commands.json         实际 C++ 编译命令
  commands.json                 每次执行的完整参数和顺序
  scenarios.csv                 场景语义、量纲、worker、可比性分类
  process_usage.csv             40 次程序执行的 CPU/墙钟耗时
  round_01/ ... round_10/
    faio_batch.csv              原始 batch 输出
    faio_sampled.csv             原始采样汇总
    tokio_batch.csv / tokio_sampled.csv
    comparison.csv              两个实现合并的该轮全部指标
    samples/faio/*.csv.gz        原始逐次延迟样本
    samples/tokio/*.csv.gz
    <category>_bar.png           每轮各类别全部指标柱状图
    <category>_line.png          每轮各类别全部指标折线图
    latency_distribution_*.png  每个场景的经验分位数曲线，含尾部
  all_rounds.csv                全部 10 轮各项数据
  summary.csv                   10 轮算术均值
  statistics.csv                均值、样本标准差、CV%、min/max、极差、中位数
  ratios.csv                    faio/Tokio 比值和可比性分类
  summary.md                    完整均值表
  mean/                         总均值柱状图和折线图
  <category>_mean_stddev.png     均值与标准差误差线
  <category>_round_variation.png 各指标跨轮波动，按自身均值归一化
```

所有采样数组在汇总前保留，不删除异常值。P99.9、最大值可能受系统抢占影响，结合 10 轮波动分析。`summary.csv` 中的 P99 是 **10 个 P99 的算术均值**，不是混合十轮样本得到的 pooled P99；两者不要混称。`ratios.csv` 的比率是 **均值之比**，不是逐轮比率的均值。高变异系数意味着当前负载/环境下结论不稳定，应先改善实验条件再比较。

旧独立基准和实验数据已移到工作区外的备份目录，位置与清单见上级 [README](../README.md)。正式对照只使用本目录的一个 C++ 文件和一个 Tokio 项目。Rust 编译产物统一在 `build/benchmark-rust/coro`，不留在测试源码目录。
