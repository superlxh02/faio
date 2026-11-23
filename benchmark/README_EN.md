# Performance benchmarks

[简体中文](README.md) | **English**

These programs measure coroutine scheduling, coroutine frame allocation, file and CPU work, user blocking pools, and TCP servers. Apart from the two TCP server programs, they print CSV to stdout, so you can redirect the output into your own scripts. Benchmarks are separate from the test suite: you run them by hand, and they do not enforce a performance budget.

The C++ programs need a C++23 compiler, spdlog, and the platform thread library. The CMake target `asio_tcp_benchmark` also needs standalone Asio. The Tokio comparisons are Rust programs whose dependencies are pinned by each `Cargo.lock`. Before comparing runtimes, fix the optimization level, worker count, and CPU set. Numbers collected with different settings are not comparable.

## Build and run

All three build systems keep their output under `target/`, in separate subdirectories, so they can coexist. CMake enables benchmarks by default (`FAIO_BUILD_BENCHMARKS`), which makes it the shortest path:

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release -j4
```

The binaries land in `target/cmake-build-linux-main-release/benchmark/`. On macOS, use the `mac-main-release` preset instead. The Linux mainline preset builds with Clang 23.

The repository pins Bazel 9.2.0. Every benchmark rule carries the `manual` tag, so `//...` skips them. Name the target you want:

```sh
bazel build --config=faio_headers //benchmark:faio_coro_benchmark
bazel run --config=faio_headers //benchmark:faio_coro_benchmark -- 100000 batch
```

Built binaries are linked under `target/bazel/bin/benchmark/`. In the Linux development container, add `--config=faio_epoll` if the container does not allow io_uring syscalls. Bzlmod fetches the pinned Asio version, so the Asio comparison needs no local headers.

xmake 3.0.1 or newer works too. Benchmarks are off by default and are only registered when the header targets are enabled, so turn them on at configure time:

```sh
xmake f -c -y -m release --faio_benchmarks=y
xmake build faio_coro_benchmark
xmake run faio_coro_benchmark 100000 batch
```

`xmake run` passes everything after the target name to the program. For `asio_tcp_benchmark`, point `FAIO_ASIO_INCLUDE_DIR` at the directory containing `asio.hpp` before configuring. Toolchain selection works the same as for the library; if the default compiler lacks C++23, follow the [build configuration](../docs/getting-started.md#xmake-构建) and pass `--toolchain=clang` with the matching `--cxx` and `--ld` paths.

Benchmark targets always build against the headers. The Bazel rules disable the module feature, and xmake registers the targets only when `--faio_headers` is on, so you do not need named modules to run them.

Each program covers a different area. All three build systems trim targets by platform: Windows has no `faio_current_thread_benchmark` or `tcp_connect_load`. In the CMake build, `asio_tcp_benchmark` is also built only when Asio is found.

| Program | Coverage |
| --- | --- |
| `faio_coro_benchmark` | Tasks, switching, synchronization primitives, concurrent workflows |
| `faio_current_thread_benchmark` | Single-thread runtime and blocking pool |
| `faio_frame_allocation_benchmark` | Cached coroutine frame allocation |
| `faio_frame_allocation_new_benchmark` | Frame allocation through global new/delete |
| `faio_mpsc_ready_queue_benchmark` | Multi-producer ready queue |
| `faio_io_cpu_benchmark` | Regular files and CPU-bound tasks |
| `faio_blocking_pool_benchmark` | Fixed and elastic user blocking pools |
| `faio_tcp_benchmark`, `asio_tcp_benchmark` | TCP server comparison |
| `tcp_echo_load` | Persistent-connection echo client, reports QPS and latency |
| `tcp_connect_load` | Short-connection client that sends one request per connection |

The three Tokio comparison projects live under `benchmark/*/tokio-benchmark/`. Run Cargo from the repository root or from those directories, and all binaries end up in `target/cargo/release/`.

## Coroutines and frame allocation

Start with the CMake binaries:

```sh
./target/cmake-build-linux-main-release/benchmark/faio_coro_benchmark 100000 batch
./target/cmake-build-linux-main-release/benchmark/faio_coro_benchmark 100000 sampled
./target/cmake-build-linux-main-release/benchmark/faio_frame_allocation_benchmark 2000000
./target/cmake-build-linux-main-release/benchmark/faio_frame_allocation_new_benchmark 2000000
cargo build --manifest-path benchmark/coro/tokio-benchmark/Cargo.toml --release --locked
```

`batch` divides total time by the number of operations. `sampled` reads the clock per operation and reports percentiles. Do not mix the two when ranking implementations; interpret them separately. The [coroutine benchmark guide](coro/README.md) explains what one operation means in each scenario. Bazel and xmake take the same arguments: replace the executable with `bazel run --config=faio_headers //benchmark:faio_coro_benchmark -- 100000 batch` or `xmake run faio_coro_benchmark 100000 batch`.

The two frame programs allocate through a cache and through global `new` respectively. On Linux you can point `LD_PRELOAD` at an installed tcmalloc shared library to compare against the system allocator. Both use separate workload translation units and disable LTO so the compiler cannot remove the allocations.

## Files and CPU

```sh
mkdir -p target/perf-files
./target/cmake-build-linux-main-release/benchmark/faio_io_cpu_benchmark target/perf-files 4096 4 all
cargo build --manifest-path benchmark/io/tokio-benchmark/Cargo.toml --release --locked
```

The last argument selects `io`, `cpu`, or `all`, and `4096` is the number of 4 KiB operations. The directory must already exist; the program creates and removes its own test files inside it. Throughput from a warm page cache, from cold storage, and for durable writes are three different measurements, so say which one you report.

## User blocking pool

```sh
./target/cmake-build-linux-main-release/benchmark/faio_blocking_pool_benchmark fixed 64
./target/cmake-build-linux-main-release/benchmark/faio_blocking_pool_benchmark elastic 64
```

Each policy measures construction and teardown of an idle pool plus a current-thread runtime. Dispatch latency is timestamped immediately before submission and ends when the closure starts. Direct submissions and `spawn_blocking` submissions both use a sparse 2 ms interval. Cold bursts arrive every 90 ms, and the elastic pool keeps threads alive for 50 ms, so one run captures several reclamation cycles. A cold burst submits 16 jobs back to back, each blocking for 1 ms. Warmed throughput uses a minimal job and a job with 20000 rounds of integer work.

`blocking_threads` in the CSV counts only user blocking-pool threads. Burst rows record the maximum thread count before the burst and after the whole batch is submitted.

## TCP

Run the server and the client in separate terminals:

```sh
./target/cmake-build-linux-main-release/benchmark/faio_tcp_benchmark 127.0.0.1 9000 4 echo
```

```sh
./target/cmake-build-linux-main-release/benchmark/tcp_echo_load 127.0.0.1 9000 4 64 64 10 1
```

The client arguments are host, port, client threads, connections, payload bytes, measurement seconds, and warmup seconds. Output reports checksum-validated QPS, mean latency, P99, and error count. The short-connection client `tcp_connect_load` takes host, port, concurrent clients, payload bytes, measurement seconds, and warmup seconds.

The Tokio server:

```sh
cargo build --manifest-path benchmark/tcp/tokio-benchmark/Cargo.toml --release --locked
```

## Measurement rules

Fix the protocol, workload, worker count, CPU affinity, warmup, and optimization flags, and run implementations one at a time. Keep profilers, sanitizers, compilation, and plotting outside the timed section. Use a fresh process for each measurement and save the per-run CSV, source and binary digests, mean, sample standard deviation, and CV. Keep outliers instead of deleting them.

QPS counts requests completed inside the window. Latency samples also include slow requests that start inside the window and finish after it. The mean of per-run P99 values is not the P99 of the pooled samples. When a run reports errors or timeouts, record the failure and state how far the incomplete data extends.
