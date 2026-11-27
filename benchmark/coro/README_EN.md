# Coroutine and frame-allocation benchmarks

[简体中文](README.md) | **English**

`faio_coro_benchmark.cpp` and `tokio-benchmark/` run the same workflows: ready tasks, yielding, spawn/join, synchronization primitives, and structured concurrency. Both sides validate results and operation counts, so the comparison always uses the same workload.

## Build and run

With CMake, build only the coroutine benchmark:

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release --target faio_coro_benchmark -j4
./target/cmake-build-linux-main-release/benchmark/faio_coro_benchmark 100000 batch
./target/cmake-build-linux-main-release/benchmark/faio_coro_benchmark 100000 sampled
cargo build --manifest-path benchmark/coro/tokio-benchmark/Cargo.toml --release --locked
```

The sources sit in `benchmark/coro/`, but the benchmark targets are registered one level up. Bazel has no `benchmark/coro` package here, so the target belongs to `//benchmark`:

```sh
bazel run --config=faio_headers //benchmark:faio_coro_benchmark -- 100000 sampled
```

xmake identifies the same program by target name:

```sh
xmake f -c -y -m release --faio_benchmarks=y
xmake run faio_coro_benchmark 100000 sampled
```

All three build systems build the coroutine benchmark against the header-only library, so faio module support is not required. The frame-allocation programs are separate targets; see below.

## Two ways to measure

| Mode | Output | Meaning |
| --- | --- | --- |
| `batch` | ns/op, ops/s | Total time divided by the number of operations, with no per-operation clock reads |
| `sampled` | Mean latency and P50/P90/P99, among others | Per-operation samples that include clock and recording overhead |

One operation can mean one yield, one message, one round trip, or a group of 32 tasks, depending on the scenario. Do not add the nanosecond figures across rows or fold them into a single ranking. `timer_calibration` only calibrates timing overhead and does not decide which runtime wins.

## Workflows covered

- Ready tasks, yield, spawn/join, and the `block_on` entry point on an ordinary thread.
- Handoff within a worker, handoff across runtimes, and external notification.
- Uncontended and contended mutex, semaphore, condition_variable, latch, and barrier.
- Immediate completion, backpressure, and multi-producer delivery through MPSC.
- Startup, joining, and draining for join, join_all, scope, and select.

## Caveats when comparing with Tokio

Tokio has no one-to-one type for every case. The condition-variable and latch comparisons combine Notify with counters, and scope uses JoinSet. They check successful workflows, not identical API semantics. Rust ready futures may be inlined, while a C++ lazy task usually owns a separate coroutine frame. That difference shows up in allocation and switching costs.

## Frame allocation and the other programs

`current_thread_benchmark.cpp` compares driving from the calling thread against driving from one background worker. `frame_allocation_benchmark.cpp` creates small frames, 512-byte frames, and 4 KiB frames through factories in a separate translation unit, and it measures burst allocation as well. The cached and global-`new` variants are separate targets, both built without LTO:

```sh
./target/cmake-build-linux-main-release/benchmark/faio_frame_allocation_benchmark 2000000
./target/cmake-build-linux-main-release/benchmark/faio_frame_allocation_new_benchmark 2000000
```

In Bazel the targets are `//benchmark:faio_frame_allocation_benchmark` and `//benchmark:faio_frame_allocation_new_benchmark`; in xmake they are `faio_frame_allocation_benchmark` and `faio_frame_allocation_new_benchmark`. On Linux, point `LD_PRELOAD` at tcmalloc for the global-`new` variant to see what changes when the allocator is replaced.

## What makes results comparable

Worker count, CPU set, compiler, and idle policy all affect the numbers. Record the configuration you used, keep warmup separate from the measured rounds, and retain the data and variation from every run. The [benchmark guide](../README.md) has the full program list and measurement rules.
