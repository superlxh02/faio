# HTTP Benchmark Compare

| Runtime | Requests/sec | Avg(ms) | P50(ms) | P90(ms) | P99(ms) | Timeout | Transfer/sec |
|---|---:|---:|---:|---:|---:|---:|---:|
| Go(Gin) | 447991.66 | 9.61 | 8.14 | 17.18 | 39.14 | 0 | 58.96MB |
| C++(faio) | 638900.79 | 8.58 | 4.25 | 12.81 | 32.53 | 0 | 81.04MB |
| C++(beast) | 257148.38 | 18.52 | 18.77 | 20.14 | 24.52 | 2940 | 25.26MB |
