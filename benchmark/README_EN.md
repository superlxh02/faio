# Performance benchmarks

[简体中文](README.md) | **English**

This directory contains coroutine, file, CPU and TCP benchmarks, plus Tokio comparison projects pinned by Cargo.lock. Benchmarks emit CSV and run separately from unit tests.

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release -j4
./build/linux-main-release/benchmark/faio_coro_benchmark 100000 batch
./build/linux-main-release/benchmark/faio_coro_benchmark 100000 sampled
```

The mainline preset uses Clang 23. Provide spdlog, standalone Asio and the selected platform libraries. Tokio comparisons also need Rust/Cargo:

```sh
cargo build --manifest-path benchmark/coro/tokio-benchmark/Cargo.toml --release --locked
cargo build --manifest-path benchmark/io/tokio-benchmark/Cargo.toml --release --locked
cargo build --manifest-path benchmark/tcp/tokio-benchmark/Cargo.toml --release --locked
```

Available C++ programs include:

- `faio_coro_benchmark`, `faio_current_thread_benchmark` and `faio_mpsc_ready_queue_benchmark`.
- `faio_frame_allocation_benchmark` and `faio_frame_allocation_new_benchmark` for cached frames versus global new/delete.
- `faio_io_cpu_benchmark DIRECTORY COUNT WORKERS all|io|cpu`.
- `faio_blocking_pool_benchmark fixed|elastic [THREAD_LIMIT]` measures startup, teardown, dispatch latency, and warmed throughput.
- `faio_tcp_benchmark`, `asio_tcp_benchmark`, `tcp_echo_load` and `tcp_connect_load`.

TCP echo example, in separate terminals:

```sh
./build/linux-main-release/benchmark/faio_tcp_benchmark 127.0.0.1 9000 4 echo
```

```sh
./build/linux-main-release/benchmark/tcp_echo_load 127.0.0.1 9000 4 64 64 10 1
```

The client arguments are host, port, client threads, connections, payload bytes, measurement seconds and warmup seconds. It validates echo data and reports QPS, mean latency, percentiles and errors.

Use identical workloads, compiler optimization, worker counts, CPU sets and timing windows. Run implementations serially and keep profilers, sanitizers, compilation and plotting outside timed measurements. Save every independent run, including errors and outliers. A mean of per-run P99 values is not the percentile of pooled samples. Cached file I/O is not cold-storage or durable-write throughput.

See the [Chinese guide](README.md) and [coroutine protocol](coro/README.md) for details.
