# Benchmark

[简体中文](README.md) | **English**

## 1. Directory Layout and Unified Entry Points

```text
benchmark/
  CMakeLists.txt
  README.md
  coro/
    faio_coro_benchmark.cpp      all faio coroutine tests in a single C++ file
    tokio-benchmark/             matching Tokio project; src/main.rs is the test implementation
    README.md                    detailed coroutine protocol and comparison semantics
  tcp/
    faio_tcp_benchmark.cpp
    asio_tcp_benchmark.cpp
    tokio-benchmark/
  result/
    coro/                        completed official coroutine data
    tcp/                         generated when the TCP script runs later
```

| Suite | Implementation                                                            | Fixed Rounds | Entry Point                                  |
|-------|---------------------------------------------------------------------------|-------------:|----------------------------------------------|
| coro  | faio / Tokio, 43 scenarios, each round runs both batch and sampled passes |           10 | `python3 scripts/compare_coro_benchmarks.py` |
| tcp   | faio / standalone Asio / Tokio                                            |            3 | `python3 scripts/compare_tcp_benchmarks.py`  |

`benchmark_report.py` is the shared statistics/plotting module, and `network_benchmark.py` is the TCP runner; both live
in `scripts` and are not additional test entry points. C++ artifacts are located in `build/benchmark-o2/benchmark`, and
Rust artifacts in `build/benchmark-rust/<coro|tcp>`.

The old standalone `coroutine_stress.cpp`, `coroutine_primitives.cpp`, `coroutine_scheduler.cpp`, and the old experiment
scripts have been archived; the official coroutine comparison consists of only one C++ file and one Tokio project in
`coro`.

## 2. How These Tests Are Planned and Written

### 2.1 Define the Question First, Then the Operation Unit

Tests are written at three levels:

1. **Basic paths**: directly awaiting a child task, yield, uncontended synchronization primitives, observing the cost of
   the coroutine wrapper and the immediately-complete path.
2. **Contention and concurrent workflows**: lock contention, task groups, cross-thread notification, multi-producer
   MPSC, observing the real cost of enqueueing, backpressure, wakeup, task lifecycle, and result collection.
3. **Network end-to-end paths**: real client requests including protocol parsing, application response, I/O, and
   scheduling, observing overall service throughput and request latency.

Each scenario first defines what "one operation" is: one yield, a group of 32 tasks, one message, one bidirectional
handoff, or one HTTP request. ns/op and ops/sec across different units are not directly arranged into an overall
ranking.

### 2.2 Implement the Same Workflow on Both Sides

- Worker count, participant count, message size, queue capacity, and the actual operation count are identical. The MPSC
  pipeline uses 16-byte messages on both sides; the network service body is uniformly 16 bytes.
- Do not compare interface names only: faio join starts an independent branch, so the Tokio counterpart spawns first and
  then awaits the handle; the select counterpart also drains both submitted branches.
- Tokio has no native condition variable/latch; the same successful workflows are built with Mutex+Notify and
  counter+Notify respectively, explicitly labeled as composite references.
- Work executed inside a worker is not run directly on the Rust host block_on thread. The cross-thread RTT uses two
  single-worker runtimes to ensure the threads at the two ends differ.
- Programs validate message/task results and completion counts; scripts check the scenario list, operation counts, and
  numeric validity. Errors/timeouts do not participate in incomplete means.

### 2.3 Separate Timing from Auxiliary Work

- Runtime creation/shutdown, allocation of the main sample arrays, statistics, and file output are placed outside the
  corresponding timed regions; task group/queue operations required by a specific scenario are timed as defined.
- Each coroutine process runs a full warmup first; each network service first runs one warmup load. Warmup data is saved
  or discarded, but never participates in the means.
- **batch** disables per-operation clock reads and only divides the total elapsed time by the count, observing the
  amortized cost; loop and validation auxiliary operations are kept, and no claim of theoretical minimum instruction
  cost is made.
- **sampled** reads the clock on every operation and records complete latencies, outputting P50/P90/P99/P99.9/max;
  per-operation latency and concurrent wall-clock amortized cost are not the same concept.
- A separate clock calibration scenario is kept, and no forced subtraction is applied to nanosecond-level results. Clock
  discretization may make the sampled P50 equal to 0; it should be interpreted together with the batch data.

### 2.4 Collect Multiple Rounds, Then Aggregate and Plot

The two sides are measured serially with alternating order; the two suites share a process lock. Python
compression/plotting runs only after all measurements complete, reducing chart generation interference with the CPU.

Results use the **arithmetic mean** across rounds, while also saving the sample standard deviation (ddof=1), CV%,
minimum, maximum, range, and median. The overall P99 result is the "mean of per-round P99", not the P99 of all samples
merged together. Per-round data and all raw coroutine latencies are available for review; outliers are not deleted.

## 3. Environment, Build, and Run

Linux, GCC, CMake, liburing, spdlog, standalone Asio, GoogleTest, rustc/cargo, wrk, and Python pandas/numpy/matplotlib.

C++ is fixed at GCC `-O2 -DNDEBUG` with LTO disabled, using the separate `build/benchmark-o2`; scripts check that the
compiler is actually GNU. Rust uses rustc; both projects use `opt-level=2`, `lto=false`, `codegen-units=1`, with
dependencies pinned by Cargo.lock.

```bash
# Run the full coroutine benchmark on Linux
python3 scripts/compare_coro_benchmarks.py
# Use the existing Docker environment, allowing io_uring syscalls
docker run --rm --security-opt seccomp=unconfined \
  -v "$PWD:/workspace/faio" -w /workspace/faio \
  faio:dev python3 scripts/compare_coro_benchmarks.py
```

Coroutines default to count=100000 and are pinned to CPU set `0-5`; `--count` and `--cpus` can specify the local load
and allowed CPUs. Most single operations run count times; handoff/notification/block_on use min (count,10000), and the
32-task groups run floor (count/32) times. The CSV `operations` field stores the real counts.

```bash
# The network script is kept for on-demand runs and is not triggered by the coroutine entry
python3 scripts/compare_tcp_benchmarks.py
# Adjusting the load does not change the fixed three-round statistics rules
python3 scripts/compare_tcp_benchmarks.py --connections 512 --duration 20s --server-workers 4
```

Historical coroutine results and current TCP echo results are retained separately. The epoll/kqueue TCP comparison uses
`scripts/compare_tcp_echo.py`; initial Linux measurements and diagnostics are listed in
the [TCP echo measurement log](result/tcp_echo/README.md). Initial quick runs fail the 20% target; each directory's
`acceptance.json` records the actual outcome.

## 4. All Coroutine Test Items

The tables below list the **43 scenarios** actually output by the program, item by item. `w` denotes worker count, `p`
denotes producer/participant count, `c` denotes capacity, and `k` denotes permit count. "Overhead" refers to batch total
elapsed time/count; "latency" refers to the sampled single-operation timestamp interval.

### Basics and Timing Reference

| Test Item           | Configuration | Operation Unit                | What Is Measured / How to Interpret                                                                                                                     |
|---------------------|---------------|-------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------|
| `timer_calibration` | host          | one call / complete operation | Reference cost of consecutive clock reads and sample writes; not used as a win/loss metric of runtime performance.                                      |
| `task_await_ready`  | 1 worker      | one call / complete operation | Awaiting a child task that returns 1 until the result is obtained; the C++ coroutine frame and the inlinable Rust future are language-model references. |

### Coroutine Switching and Wakeup

| Test Item                          | Configuration             | Operation Unit                | What Is Measured / How to Interpret                                                                                                                                                                              |
|------------------------------------|---------------------------|-------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `yield_1`                          | 1 worker                  | one call / complete operation | The current task yields until it resumes; 1 worker guarantees the same thread, 4 workers do not guarantee migration, and migration counts are recorded separately.                                               |
| `handoff_rtt_w1`                   | 1 worker                  | one round trip                | Two tasks alternate using request/reply semaphores; bidirectional RTT from request release to reply acquisition. w1 guarantees the same thread, w4 does not guarantee cross-thread.                              |
| `yield_4`                          | 4 workers                 | one call / complete operation | The current task yields until it resumes; 1 worker guarantees the same thread, 4 workers do not guarantee migration, and migration counts are recorded separately.                                               |
| `handoff_rtt_w4`                   | 4 workers                 | one round trip                | Two tasks alternate using request/reply semaphores; bidirectional RTT from request release to reply acquisition. w1 guarantees the same thread, w4 does not guarantee cross-thread.                              |
| `cross_runtime_rtt`                | 2 runtimes, 1 worker each | one round trip                | Two independent single-worker runtimes ensure the threads at the two ends differ; two-semaphore handoff RTT, not treated as one-way latency.                                                                     |
| `external_notification_registered` | 1 worker                  | one notification              | An external helper confirms the waiting node is registered before releasing the permit, measuring release to coroutine resumption; the helper busy-polls, excluding the fast path where a permit already exists. |

### Synchronization Contention

| Test Item                       | Configuration                        | Operation Unit                | What Is Measured / How to Interpret                                                                                                                        |
|---------------------------------|--------------------------------------|-------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `mutex_contention_p4_w1`        | 1 worker, 4 participants             | one call / complete operation | 4 tasks contend for the same lock, yielding while holding the lock after each acquisition and then unlocking; latency includes lock waiting and switching. |
| `semaphore_contention_k2_p4_w1` | 1 worker, 4 participants, 2 permits  | one call / complete operation | 4 tasks contend for 2 permits, yielding while holding a permit and then releasing; observes backpressure waiting and scheduling.                           |
| `barrier_p4_w1`                 | 1 worker, 4 participants             | one barrier arrival           | 4 tasks repeatedly join a four-party barrier, each running count/4 times; one operation is one participant arriving and resuming.                          |
| `mutex_contention_p4_w4`        | 4 workers, 4 participants            | one call / complete operation | 4 tasks contend for the same lock, yielding while holding the lock after each acquisition and then unlocking; latency includes lock waiting and switching. |
| `semaphore_contention_k2_p4_w4` | 4 workers, 4 participants, 2 permits | one call / complete operation | 4 tasks contend for 2 permits, yielding while holding a permit and then releasing; observes backpressure waiting and scheduling.                           |
| `barrier_p4_w4`                 | 4 workers, 4 participants            | one barrier arrival           | 4 tasks repeatedly join a four-party barrier, each running count/4 times; one operation is one participant arriving and resuming.                          |

### Synchronization Composite References

| Test Item           | Configuration | Operation Unit                | What Is Measured / How to Interpret                                                                                                                  |
|---------------------|---------------|-------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------|
| `cv_roundtrip_w1`   | 1 worker      | one round trip                | Predicate handshake: modify state while holding the lock, notify, wait for the reply and reacquire the lock; Tokio Mutex+Notify composite reference. |
| `latch_fanin_32_w1` | 1 worker      | one group of 32 tasks         | Create a latch with count 32, 32 tasks count_down, wait for it to open and drain the handles; Tokio AtomicUsize+Notify composite reference.          |
| `cv_roundtrip_w4`   | 4 workers     | one round trip                | Predicate handshake: modify state while holding the lock, notify, wait for the reply and reacquire the lock; Tokio Mutex+Notify composite reference. |
| `latch_fanin_32_w4` | 4 workers     | one group of 32 tasks         | Create a latch with count 32, 32 tasks count_down, wait for it to open and drain the handles; Tokio AtomicUsize+Notify composite reference.          |
| `latch_ready`       | 1 worker      | one call / complete operation | Wait on a latch already at zero; Tokio counter-check composite reference, no native latch.                                                           |

### Concurrency Interfaces

| Test Item              | Configuration | Operation Unit                | What Is Measured / How to Interpret                                                                                                                               |
|------------------------|---------------|-------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `spawn_join_1`         | 1 worker      | one call / complete operation | Spawn a task returning 1, await the JoinHandle until the result is obtained; includes task creation, enqueueing, and result delivery.                             |
| `spawn_join_4`         | 4 workers     | one call / complete operation | Spawn a task returning 1, await the JoinHandle until the result is obtained; includes task creation, enqueueing, and result delivery.                             |
| `join_all_32`          | 1 worker      | one group of 32 tasks         | Create and start 32 tasks, collect all results in order and verify the count is 32; one operation is one group.                                                   |
| `scope_32`             | 1 worker      | one group of 32 tasks         | Create a scope, submit 32 tasks and wait for all to finish; Tokio JoinSet successful-completion workflow reference, no claim of identical cancellation semantics. |
| `join_ready_2`         | 1 worker      | one group of 2 tasks          | Start two independent tasks and obtain both results; Tokio spawns first then join!, one operation is one group.                                                   |
| `select_spawn_drain_2` | 1 worker      | one group of 2 tasks          | Submit two immediately-completing tasks, select the winner and drain the other branch; Tokio selects between JoinHandles then awaits the other handle.            |

### MPSC Queues

| Test Item          | Configuration                         | Operation Unit        | What Is Measured / How to Interpret                                                                                                                                    |
|--------------------|---------------------------------------|-----------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `mpsc_p1_w1_c64`   | 1 worker, 1 producer, 64 capacity     | one message           | One consumer; producer/worker/capacity configured per the name; 16-byte messages, send-to-receive latency includes backpressure and queueing; total count is verified. |
| `mpsc_p4_w1_c64`   | 1 worker, 4 producers, 64 capacity    | one message           | One consumer; producer/worker/capacity configured per the name; 16-byte messages, send-to-receive latency includes backpressure and queueing; total count is verified. |
| `mpsc_p1_w1_c1024` | 1 worker, 1 producer, 1024 capacity   | one message           | One consumer; producer/worker/capacity configured per the name; 16-byte messages, send-to-receive latency includes backpressure and queueing; total count is verified. |
| `mpsc_p4_w1_c1024` | 1 worker, 4 producers, 1024 capacity  | one message           | One consumer; producer/worker/capacity configured per the name; 16-byte messages, send-to-receive latency includes backpressure and queueing; total count is verified. |
| `mpsc_p1_w4_c64`   | 4 workers, 1 producer, 64 capacity    | one message           | One consumer; producer/worker/capacity configured per the name; 16-byte messages, send-to-receive latency includes backpressure and queueing; total count is verified. |
| `mpsc_p4_w4_c64`   | 4 workers, 4 producers, 64 capacity   | one message           | One consumer; producer/worker/capacity configured per the name; 16-byte messages, send-to-receive latency includes backpressure and queueing; total count is verified. |
| `mpsc_p1_w4_c1024` | 4 workers, 1 producer, 1024 capacity  | one message           | One consumer; producer/worker/capacity configured per the name; 16-byte messages, send-to-receive latency includes backpressure and queueing; total count is verified. |
| `mpsc_p4_w4_c1024` | 4 workers, 4 producers, 1024 capacity | one message           | One consumer; producer/worker/capacity configured per the name; 16-byte messages, send-to-receive latency includes backpressure and queueing; total count is verified. |
| `mpsc_ready_64`    | 1 worker, 64 capacity                 | one send/receive pair | The same task performs async send+recv on a queue with capacity 64; 8-byte payload, one operation is one pair, no inter-coroutine switching.                           |
| `mpsc_try_64`      | 1 worker, 64 capacity                 | one send/receive pair | The same task with capacity 64, try_send+try_recv; 8-byte payload, one operation is one pair, measuring the non-suspending path.                                       |

### Task Submission

| Test Item          | Configuration | Operation Unit       | What Is Measured / How to Interpret                                                                                                                         |
|--------------------|---------------|----------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `external_burst_1` | 1 worker      | one task             | host batch spawn_detached, measuring queueing latency from before submission to task start; the whole span ends when the all-tasks-complete signal arrives. |
| `internal_burst_1` | 1 worker      | one task             | in-worker batch spawn_detached, measuring queueing latency from before submission to task start; observes the local queue and the overflow path.            |
| `external_burst_4` | 4 workers     | one task             | host batch spawn_detached, measuring queueing latency from before submission to task start; the whole span ends when the all-tasks-complete signal arrives. |
| `internal_burst_4` | 4 workers     | one task             | in-worker batch spawn_detached, measuring queueing latency from before submission to task start; observes the local queue and the overflow path.            |
| `block_on_entry`   | 1 worker      | one entry round trip | host submits a task to a worker and waits for it to return 1; Tokio uses block_on(spawn(task)), measuring host→worker→host.                                 |

### Synchronization Immediately-Complete Paths

| Test Item         | Configuration | Operation Unit                | What Is Measured / How to Interpret                                                                    |
|-------------------|---------------|-------------------------------|--------------------------------------------------------------------------------------------------------|
| `semaphore_ready` | 1 worker      | one call / complete operation | One permit already available, the uncontended path of a single task's acquire+release.                 |
| `mutex_ready`     | 1 worker      | one call / complete operation | The uncontended path of a single task's lock+unlock; the Tokio guard releases the lock on destruction. |
| `barrier_ready_1` | 1 worker      | one call / complete operation | Single-participant barrier arrive_and_wait, measuring the immediately-complete path.                   |

### Coroutine Metric Meanings

| CSV Field                                                | Unit                   | Meaning                                                                                                                       |
|----------------------------------------------------------|------------------------|-------------------------------------------------------------------------------------------------------------------------------|
| `batch_ns_per_op`                                        | ns / scenario unit     | Amortized wall-clock cost with per-operation clock reads disabled; lower is better                                            |
| `sampled_ns_per_op`                                      | ns / scenario unit     | Amortized cost including per-operation sampling; not presented as net switching cost                                          |
| `ops_per_sec`                                            | scenario unit / second | 1e9 / batch_ns_per_op; for grouped scenarios it is groups/second                                                              |
| `p50_ns`, `p90_ns`, `p99_ns`, `p999_ns`                  | ns                     | 50%, 90%, 99%, 99.9% percentiles of the scenario's operation/message latency                                                  |
| `max_ns`                                                 | ns                     | Maximum sample latency of this round, including tails caused by system preemption                                             |
| `migrations`, `batch_migrations`                         | count                  | Number of worker/thread changes before and after a yield; from sampled and batch respectively; not a CPU core migration count |
| `operations`                                             | count                  | Actual number of operations/messages/groups in this round; interpreted together with the operation unit                       |
| `workers`, `producers`, `capacity`, `unit`, `comparison` | configuration / label  | Describe the workload and semantic comparability; not metrics for comparing speed                                             |

For more details on lifecycle, backpressure, sampling protocol, and Tokio composite differences,
see [coro/README.md](coro/README.md). Currently select only tests immediately-completing branches; cancellation slow
paths, exception propagation, and fairness are separate semantic validations, and no claim is made that these
performance loads cover them.

## 5. All TCP Test Items and Implementations

| Implementation | Test Code                         | Implementation Path                                                       |
|----------------|-----------------------------------|---------------------------------------------------------------------------|
| faio           | `tcp/faio_tcp_benchmark.cpp`      | TcpListener accepts; one faio task per connection; async read / write_all |
| Asio           | `tcp/asio_tcp_benchmark.cpp`      | co_spawn, one awaitable per connection, async_read_some / async_write     |
| Tokio          | `tcp/tokio-benchmark/src/main.rs` | tokio::spawn, TcpStream read / write_all                                  |

All three execute the same **`keep_alive_get_index`** load: local wrk sends HTTP/1.1 GET /index, frames are split over
TCP by `\r\n\r\n`, a fixed response is returned, and the next request on the same connection continues to be read. The
body `hello benchmark\n` is 16 bytes, with a consistent Content-Length. It measures the complete path of TCP I/O +
coroutine handling of a fixed request/response, and is not a comparison of full HTTP implementations.

Program writing order: accept loop → start a per-connection task → read into buffer → find the request boundary → write
back completely → keep subsequent requests. The runner first checks the service status code/body, then warms up,
collects wrk metrics, and shuts down the service. The 11 metrics below are output for every implementation in every
round.

### TCP Complete Metrics Table

| Test Item / CSV Field    | Unit            | Meaning and Comparison Direction                                                               |
|--------------------------|-----------------|------------------------------------------------------------------------------------------------|
| `requests_per_sec`       | requests/second | wrk completed-request throughput; higher is better, and error counts must be checked together  |
| `latency_avg_ms`         | ms              | Average request latency reported by wrk; lower is better                                       |
| `latency_p50_ms`         | ms              | Latency not exceeded by half of the requests; observes common latency                          |
| `latency_p90_ms`         | ms              | 90% percentile; observes slower requests                                                       |
| `latency_p99_ms`         | ms              | 99% percentile; observes tail latency                                                          |
| `timeout_count`          | count           | Number of wrk timeouts; fewer is better                                                        |
| `connect_errors`         | count           | Number of connection-establishment failures; cannot be regarded as completed requests          |
| `read_errors`            | count           | Number of response-read failures                                                               |
| `write_errors`           | count           | Number of request-send failures                                                                |
| `non_success_count`      | count           | Number of non-2xx/3xx responses reported by wrk; this load should normally return 200          |
| `transfer_bytes_per_sec` | bytes/second    | wrk Transfer/sec uniformly converted to bytes; includes HTTP headers, not pure body throughput |

By default, each implementation in each round first warms up with `wrk -t4 -c5000 -d3s`, then collects with
`wrk --latency -t4 -c5000 -d60s`, for a full 3 rounds. The three implementations rotate order across rounds, each
measured once at the first/middle/last position.

wrk is a closed-loop load, with same-machine client/server competing for CPU. Complete stdout and text histograms are
saved; wrk does not output per-request timestamps, so no fabricated per-request CSV or P99.9 is generated here.
Three-round variation can only serve as a rough stability observation and is not presented as a precise confidence
interval.

## 6. All Results and Charts

Each run outputs to a new directory `benchmark/result/<suite>/<UTC time>/`, and `latest.txt` is updated on success.
Stopped/failed runs do not form complete means and do not update latest.

| File / Chart                                              | Content                                                                                                              |
|-----------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------|
| `round_XX/comparison.csv`                                 | All implementations, scenarios, and metrics of this round                                                            |
| `round_XX/<category>_bar.png`                             | Bar charts of all numeric performance metrics per category in this round                                             |
| `round_XX/<category>_line.png`                            | Comparison lines arranged by scenario in this round; does not represent time evolution                               |
| `round_XX/samples/<faio                                   | tokio>/*.csv.gz`                                                                                                     | All per-operation raw coroutine latencies, kept in collection order |
| `round_XX/latency_distribution_*.png`                     | Empirical percentile curves per coroutine scenario, including the tail                                               |
| `round_XX/<implementation>_{warmup,measured}.txt`         | Raw warmup and measured wrk output for the network                                                                   |
| `round_XX/<implementation>_server.log`                    | Network service output, for checking errors                                                                          |
| `all_rounds.csv`                                          | All numeric values of the 10/3 rounds, without outlier trimming                                                      |
| `summary.csv`, `summary.md`                               | Arithmetic mean of each metric across rounds and the complete tables                                                 |
| `statistics.csv`                                          | mean/stddev/CV/min/max/range/median for each item                                                                    |
| `mean/<category>_{bar,line}.png`                          | Overall-mean bar charts and comparison lines                                                                         |
| `<category>_mean_stddev.png`                              | Overall mean with sample standard deviation error bars                                                               |
| `<category>_round_variation.png`                          | Variation of all metrics across the 10/3 rounds, normalized by their own means                                       |
| `ratios.csv`                                              | Ratios of coroutine faio/Tokio means, with comparability labels; overhead/latency below 1 means faio is smaller      |
| `process_usage.csv`                                       | user/system/wall time of 40 complete coroutine program runs, including warmup and output; not per-scenario CPU usage |
| `metadata.json`, `commands.json`, `compile_commands.json` | Real environment, order, arguments, and compile options                                                              |
| `source_snapshot.tar.gz`                                  | Library/benchmark source code, scripts, and dependency locks at test time, to reproduce uncommitted workspace state  |

Charts are split by category and metric, each metric using its own axis; log scale is noted when the span is large. PNGs
provide visualization, CSVs keep precise values. When CV is very high, analyze the variation first; do not claim a
stable improvement based on a single mean alone.

## 7. Cleanup and Historical Data

The complete 10-round results of the new coroutine protocol are kept in `result/coro`. Old standalone programs, old
experiment scripts/raw data, old single-round network results, and stopped TCP runs have been archived outside the
workspace:

```text
/Users/lxh/workspace/cpp/faio_backups/benchmark_cleanup_20261002_110446
```

Backups are saved with their original relative paths and can be used to trace measurements in old documents. The old
5/7/9/11-round median experiments are not included in the official 10-round arithmetic means.

## Cross-platform TCP echo measurement

Configure `macos-clang23` or `linux-dual`, then run `python3 scripts/compare_tcp_echo.py --build build/<preset>`. Linux
uses the same dual-backend binary with separate `--io-backend=uring` and `--io-backend=epoll` runs; server logs must
confirm the selected backend. The `linux-epoll` preset builds without liburing. The same native POSIX client measures
both services using identical echo framing, TCP_NODELAY, 64 KiB receive buffers, server workers, and payloads
(64/1024/16384 bytes). Connection establishment and warmup are excluded; every response is byte-checked. Three
alternating rounds retain throughput, actual p50/p95/p99/p99.9 RTT, raw CSV, logs, commands, build flags, environment, a
complete source snapshot, and binary/header hashes. Every configuration must reach 80% of Tokio throughput and remain
within 120% of Tokio p50/p99 latency; `acceptance.json` records failed configurations. The process lock prevents
overlapping benchmarks, and runs require at least three measured seconds and one warmup second. Tokio is pinned to
1.49.0; both sides use O2 without LTO. Results are same-host closed-loop measurements and may be limited by the
client/shared CPU.

Run `python3 scripts/check_installed_headers.py --build build/<preset>` to validate the installed CMake package,
independent module headers, and a two-translation-unit consumer.
