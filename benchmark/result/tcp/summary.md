# TCP(HTTP-like) Benchmark Compare

| Runtime | Requests/sec | Avg(ms) | P50(ms) | P90(ms) | P99(ms) | Timeout | Transfer/sec |
|---|---:|---:|---:|---:|---:|---:|---:|
| C++(asio) | 446577.63 | 14.20 | 10.63 | 12.50 | 59.42 | 2445 | 52.81MB |
| C++(faio-tcp) | 651550.61 | 4.53 | 3.51 | 7.16 | 14.18 | 0 | 77.05MB |
| Rust(tokio) | 859649.68 | 4.23 | 3.07 | 8.22 | 14.60 | 0 | 102.48MB |
