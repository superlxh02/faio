# benchmark

本目录用于统一管理压测代码，分为 HTTP、TCP、协程三类。

## 目录结构

- `benchmark/http/faio_http_benchmark.cpp`：faio HTTP 基准服务
- `benchmark/http/beast_http_benchmark.cpp`：Boost.Beast HTTP 基准服务
- `benchmark/http/gin-http/`：Go Gin HTTP 基准服务
- `benchmark/tcp/faio_tcp_benmark.cpp`：faio TCP（HTTP-like 响应）
- `benchmark/tcp/asio_tcp_benchmark.cpp`：standalone Asio TCP（HTTP-like 响应）
- `benchmark/tcp/tokio-benchmark/`：Rust Tokio TCP（HTTP-like 响应）
- `benchmark/coroutine_stress.cpp`：协程并发压测

构建后 C++ 可执行文件位于 `build/benchmark/`。

## HTTP 基准对比（Gin vs faio vs Beast）

### 构建 C++ 目标

```bash
cmake --build build -j4 --target faio_http_benchmark beast_http_benchmark
```

### 一键运行脚本（推荐）

```bash
python3 scripts/compare_http_benchmarks.py
```

默认压测参数：`wrk -t4 -c5000 -d60s`


- `summary.csv`
- `summary.md`
- `comparison.png`

## TCP 基准对比（asio vs faio-tcp vs tokio）

### 构建 C++ 目标

```bash
cmake --build build -j4 --target asio_tcp_benchmark faio_tcp_benmark
```

### 一键运行脚本（推荐）

```bash
python3 scripts/compare_tcp_benchmarks.py
```

默认压测参数：`wrk -t4 -c5000 -d60s`


- `summary.csv`
- `summary.md`
- `comparison.png`

## 协程并发 benchmark（单独保留）

```bash
cmake --build build -j4 --target coroutine_stress
./build/benchmark/coroutine_stress [workers] [iterations_per_worker]
```

示例：

```bash
./build/benchmark/coroutine_stress 5000 5000
```

## 脚本依赖

```bash
python3 -m pip install pandas matplotlib
```
