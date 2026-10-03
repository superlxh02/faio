# Coroutine Benchmark Protocol

[简体中文](README.md) | **English**

This directory contains only one faio C++ test program and one Tokio Rust project. The entry point is
`scripts/compare_coro_benchmarks.py`, and results are written to `benchmark/result/coro/<UTC time>/`.

## 1. How to Run and Fixed Rules

```bash
python3 scripts/compare_coro_benchmarks.py
```

By default, each implementation runs twice per round, each pass covering 43 scenarios, for a full **10 rounds**. The
default is `count=100000`, and both sides are pinned to the same CPU set `0-5`; use `--cpus` to specify a set allowed on
the local machine. `--count` changes the load; it must be no less than 1024 and divisible by 4. The number of rounds is
fixed to prevent a single trial run from being treated as the official mean.

Linux Docker environment (this repository depends on io_uring):

```bash
docker run --rm --security-opt seccomp=unconfined \
  -v "$PWD:/workspace/faio" -w /workspace/faio \
  faio:dev python3 scripts/compare_coro_benchmarks.py
```

- C++: standalone CMake directory `build/benchmark-o2`; verify the actual compiler is GNU GCC; explicitly specify
  `-O2 -DNDEBUG`; disable LTO.
- Rust: Tokio is pinned to **1.49.0**; `cargo build --release --locked`; `opt-level=2`, `lto=false`, `codegen-units=1`.
  Rust uses rustc; "GCC -O2" applies only to C++.
- Both sides use a multi-threaded runtime, even with a single worker. Tokio enables the I/O/time driver, and faio
  creates its I/O engine normally; runtime initialization and shutdown are not counted in scenario timing.
- Each test process first performs a full warmup pass of `min(count,1024)` without emitting warmup data. Worker
  resumption additionally warms up 1000 yields. Different scenarios do not run in parallel.
- The order of the two implementations and the two timing modes alternates by round parity. TCP/HTTP and coroutine tests
  share a process lock to prevent tests in the same checkout from competing for CPU.
- Python plotting is not started during measurement; raw samples are compressed and plotted only after all rounds are
  collected.
- Timeouts, error returns, incorrect checksums, missing scenarios, or differing operation counts between the two sides
  fail the run; the remaining rounds are not used to fill in a mean. Incomplete runs do not update `latest.txt`.

## 2. Two Timing Modes

| Mode    | Output                                     | Purpose and boundaries                                                                                                                                                                                                                                                   |
|---------|--------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| batch   | `batch_ns_per_op`, `ops_per_sec`           | Per-operation clock reading is disabled; total wall-clock time is divided by the operation count. Loops, conditional branches, result writes, and validation are still retained; this is not claimed to be a theoretical instruction cost stripped of all auxiliary code |
| sampled | `sampled_ns_per_op`, P50/P90/P99/P99.9/max | The clock is read and a sample written for every operation; quantiles show operation latency, and the total duration includes sampling overhead                                                                                                                          |

`ns_per_op` is the amortized wall-clock cost; the sampled latency of concurrent operations may be significantly larger,
and the two must not be conflated as one metric. The timing boundaries of each scenario are shown in the table below.
Throughput is derived in batch mode as `1e9 / batch_ns_per_op`, and its unit is determined by `unit` in `scenarios.csv`:
the throughput of a group of 32 tasks is "groups/second" and must not be read as single tasks/second.

Latency uses a monotonic clock. Sample quantiles use `floor((N-1)*q)` on the sorted samples, with no interpolation. Raw
samples are written to `.csv.gz` in collection order; statistics and output files are not counted in scenario timing.
Writing sample files still affects the cache/temperature of subsequent scenarios; fixing the same protocol and rotating
the order reduces the bias but cannot eliminate it.

`timer_calibration` is the reference cost of reading the clock, looping, and writing samples, and is **not forcibly
subtracted from other results**. Virtual-machine clocks may show discrete steps of roughly tens of ns; a P50 of 0 does
not mean the operation costs nothing. Judge together with batch metrics and the raw distribution.

## 3. Scenario Matrix and Operation Definitions

`w1/w4` means 1/4 workers; `p` is the number of producers/participants; `c` is capacity; `k` is the number of permits.
All 43 scenarios are also stored in machine-readable form in each run's `scenarios.csv`.

| Scenario                           | Combinations/Count | Timing boundary per sample                                                                   | Notes                                                                                                                                                                                                                       |
|------------------------------------|-------------------:|----------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `timer_calibration`                |                  1 | Two consecutive clock reads                                                                  | Clock and sampling reference                                                                                                                                                                                                |
| `task_await_ready`                 |                  1 | Calling the child task to retrieving 1                                                       | faio lazy task symmetric transfer vs an inlinable Rust future; this is a language-model reference and cannot be used to judge scheduler superiority                                                                         |
| `yield_1`, `yield_4`               |                  2 | Yield to resumption of the current task                                                      | A single runnable task; 4 workers do **not guarantee** migration; `migrations` is reported separately                                                                                                                       |
| `handoff_rtt_w1/w4`                |                  2 | Request permit release to reply permit acquisition                                           | Two tasks on the same runtime alternate; w1 guarantees two handoffs on the same thread, w4 may run on the same or different threads                                                                                         |
| `cross_runtime_rtt`                |                  1 | Request release to reply acquisition                                                         | Two independent single-worker runtimes, guaranteeing different OS threads on both ends; this is a two-handoff round trip, and RTT/2 is not falsely claimed as a measured one-way P99                                        |
| `external_notification_registered` |                  1 | External thread releases the permit to resumption of the registered waiter                   | The armed flag is published only after the waiter has registered, excluding the fast path where a permit already exists; the external helper busy-polls for confirmation, so CPU cost differs from an ordinary idle service |
| `spawn_join_1/4`                   |                  2 | Spawning a task that returns 1 to obtaining the result from the JoinHandle                   | Creation, submission, resumption, result delivery, and handle release                                                                                                                                                       |
| `join_ready_2`                     |                  1 | Joining two tasks to collecting both results                                                 | Rust spawns two tasks first and then uses join!, matching faio's independent-branch model                                                                                                                                   |
| `join_all_32`                      |                  1 | Constructing 32 tasks to collecting all results in order                                     | Each sample is one group; group count is `floor(count/32)`; both sides validate the result sum of 32                                                                                                                        |
| `scope_32`                         |                  1 | Creating the group, submitting 32 tasks, to draining all                                     | faio scope vs Tokio JoinSet successful-completion workflow; no claim that exception/cancellation/borrow semantics are identical                                                                                             |
| `select_spawn_drain_2`             |                  1 | Submitting two immediately-completing tasks, selecting the winner, draining the other branch | Rust select! chooses between two JoinHandles and awaits the other handle; a native select! that drops plain futures cannot substitute for faio's cooperative cancellation and drain cost                                    |
| `external_burst_1/4`               |                  2 | Timestamp before host submission to child task starting execution                            | faio spawn_detached; Rust discards the JoinHandle after submission; the whole span is timed until all samples are written and the completion signal arrives                                                                 |
| `internal_burst_1/4`               |                  2 | Before worker submission to child task starting execution                                    | Batch spawn_detached inside a worker, without keeping per-result handles                                                                                                                                                    |
| `block_on_entry`                   |                  1 | Host submits, worker executes, host obtains the result                                       | Rust block_on(spawn(task)); not directly polling a Ready future on the host. Operation count is at most 10000                                                                                                               |
| `semaphore_ready`                  |                  1 | acquire to release                                                                           | One task, one existing permit, no contention                                                                                                                                                                                |
| `mutex_ready`                      |                  1 | lock to unlock                                                                               | No contention; the Rust guard unlocks on destruction                                                                                                                                                                        |
| `barrier_ready_1`                  |                  1 | Completion of one participant's arrive_and_wait                                              | Barrier immediate-completion path                                                                                                                                                                                           |
| `latch_ready`                      |                  1 | Waiting on an already-open latch completes                                                   | Tokio has no native latch; a counter check is used, which is a combination reference                                                                                                                                        |
| `mutex_contention_p4_w1/w4`        |                  2 | Lock, yield while holding, unlock                                                            | Four tasks share one lock, deliberately creating a wait queue; total operation count is count                                                                                                                               |
| `semaphore_contention_k2_p4_w1/w4` |                  2 | Acquire permit, yield while holding, release                                                 | Four tasks, two permits; must not be mixed with uncontended acquire metrics                                                                                                                                                 |
| `barrier_p4_w1/w4`                 |                  2 | Each arrival at the four-party barrier to resumption                                         | Each task runs count/4 times; all participants of the same generation must arrive; ns/op is the amortized cost per arrival                                                                                                  |
| `cv_roundtrip_w1/w4`               |                  2 | Modify predicate, notify, wait for the reply predicate, and re-acquire the lock              | faio condition_variable vs the Tokio Mutex+Notify combination; unlock after registering the notification and re-check the predicate in a loop after resumption, to avoid lost notifications                                 |
| `latch_fanin_32_w1/w4`             |                  2 | Create latch, 32 tasks count_down, wait for opening, drain handles                           | Tokio AtomicUsize+Notify single-waiter combination; each sample is one group of 32; successful-completion semantics match, but it is not equivalent to a native type                                                        |
| `mpsc_ready_64`, `mpsc_try_64`     |                  2 | One send+recv / try_send+try_recv                                                            | Same task, capacity 64, 8-byte payload, no wait queue; not a producer-consumer switching test                                                                                                                               |
| `mpsc_p{1,4}_w{1,4}_c{64,1024}`    |                  8 | Timestamp before the send call to the consumer receiving the message                         | One consumer, 1/4 producers, 1/4 workers, capacity 64/1024; 16-byte messages, both sides validate the sum of all values; latency includes backpressure, queuing, and wakeup                                                 |

Switch/condition-variable RTT, registered external notification, and block_on use `min(count,10000)`; most other
per-operation scenarios use count. 32-task groups use `floor(count/32)`, and a remainder smaller than one group is not
counted for that scenario. The CSV explicitly records the actual `operations`.

### Comparability Limitations

1. C++ tasks use independent coroutine frames; Rust ready futures may be inlined. The language difference is a fact, and
   a different Rust usage must not be adopted just to pursue identical allocation counts.
2. Tokio's CV/latch use combination implementations, labeled `tokio_notify_proxy`, `tokio_counter_notify_proxy`, and
   `tokio_counter_proxy` respectively in `comparison`; scope is labeled `joinset_proxy`. They only compare the
   successful workflows in the table.
3. The select scenario only measures successful selection and draining of two immediately-completing branches.
   Cancellation of suspended branches, error propagation, and nested scope cancellation belong to semantic validation
   and are not covered by this performance matrix.
4. All concurrency primitives measure their actual launched-execution workflows. A plain call to faio `join(...)`
   constructs a lazy task and does not itself execute concurrent work; therefore the constructed return value is not
   treated as performance data for a completed join. `co_await join(...)` and `block_on(join(...))` drive the same lazy
   combination, and the latter's entry overhead is shown by `block_on_entry`.
5. faio idle-polls 32 times by default; Tokio uses its own default scheduling/sleep policy. Lower wakeup latency may
   consume more CPU. `process_usage.csv` provides user/system/wall time for each entire test process (including warmup,
   initialization, and file output); it must not be passed off as a CPU metric for any single scenario.
6. mpsc carries timestamps instead of a Rust Instant directly, ensuring sizeof (message)=16 on both sides; batch mode
   keeps this layout but does not read the per-operation clock. Tokio's own cooperative budget stays at its default, and
   faio uses the library's current default; neither side's yielding mechanism is artificially disabled.
7. Safe cross-thread borrowing of Rust tasks is constrained by Send/'static, and some tests use Arc; faio guarantees
   reference validity through structured waiting. Such lifetime-management overhead enters the full workflow cost and
   cannot be used to infer the instruction cost of a single underlying queue operation.
8. The Docker kernel, shared host, CPU scheduling, and timing quantization all affect tail latency. These are
   observations from the current Linux ARM64 environment and are not extrapolated to x86 bare-metal conclusions. The
   actual kernel, compiler, CPU, affinity, build commands, and binary hashes are archived.

## 4. Result Files

```text
benchmark/result/coro/<timestamp>/
  metadata.json                 environment, configuration, compiler, and binary checksums
  compile_commands.json         actual C++ compile commands
  commands.json                 full arguments and order of every execution
  scenarios.csv                 scenario semantics, units, workers, comparability classification
  process_usage.csv             CPU/wall time of the 40 program executions
  round_01/ ... round_10/
    faio_batch.csv              raw batch output
    faio_sampled.csv            raw sampled summary
    tokio_batch.csv / tokio_sampled.csv
    comparison.csv              all metrics of this round for both implementations combined
    samples/faio/*.csv.gz       raw per-operation latency samples
    samples/tokio/*.csv.gz
    <category>_bar.png          per-round bar charts of all metrics per category
    <category>_line.png         per-round line charts of all metrics per category
    latency_distribution_*.png  empirical quantile curves per scenario, including the tail
  all_rounds.csv                all metrics across all 10 rounds
  summary.csv                   arithmetic means over 10 rounds
  statistics.csv                mean, sample standard deviation, CV%, min/max, range, median
  ratios.csv                    faio/Tokio ratios and comparability classification
  summary.md                    full mean table
  mean/                         overall mean bar and line charts
  <category>_mean_stddev.png    means with standard deviation error bars
  <category>_round_variation.png per-metric variation across rounds, normalized by its own mean
```

All sample arrays are retained before aggregation; outliers are not deleted. P99.9 and max values may be affected by
system preemption; analyze them together with the variation across the 10 rounds. The P99 in `summary.csv` is the
**arithmetic mean of the 10 P99 values**, not a pooled P99 computed by mixing the samples of all ten rounds; do not
conflate the two. The ratios in `ratios.csv` are **ratios of means**, not the mean of per-round ratios. A high
coefficient of variation means the conclusion is unstable under the current load/environment; improve the experimental
conditions before comparing.

The old standalone benchmarks and experimental data have been moved to a backup directory outside the workspace; see the
parent [README](../README.md) for its location and manifest. Official comparisons use only the one C++ file and one
Tokio project in this directory. Rust build artifacts are kept in `build/benchmark-rust/coro` and not left in the test
source directory.
