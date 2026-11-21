# Coroutine and frame-allocation benchmarks

[简体中文](README.md) | **English**

`faio_coro_benchmark.cpp` and `tokio-benchmark/` compare ready tasks, yielding, spawn/join, synchronization, MPSC and structured concurrency. Both validate workload results and actual operation counts.

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release --target faio_coro_benchmark -j4
./build/linux-main-release/benchmark/faio_coro_benchmark 100000 batch
./build/linux-main-release/benchmark/faio_coro_benchmark 100000 sampled
cargo build --manifest-path benchmark/coro/tokio-benchmark/Cargo.toml --release --locked
```

`batch` measures amortized wall time without a clock read per operation. `sampled` records operation latency and percentiles, including timing overhead. One operation can be a yield, message, round trip or group of tasks; different units must not be merged into an overall ranking.

Tokio condition-variable/latch references use Notify and counters; scope uses JoinSet. These compare successful workflows, not identical APIs. Rust ready futures may inline while C++ lazy tasks usually own separate frames.

`current_thread_benchmark.cpp` compares caller-thread driving with one background worker. Frame allocation uses separate workload translation units, no LTO, multiple frame sizes and burst lifetimes. Cached and global-new programs are separate targets; Linux allocator interposition can compare global new with tcmalloc.

Fix worker counts, CPU sets, optimizations and idle policies. Keep warmup and independent runs separate, retain all outliers and report variability. See the [benchmark guide](../README.md).
