/**
 * @file benchmark/io/faio_io_cpu_benchmark.cpp
 * @brief 文件 I/O 与 CPU 任务性能基准。
 */
#include "faio/detail/runtime/context.hpp"
#include "faio/fs/file.hpp"
#include "faio/log/logger.hpp"
#include "faio/runtime/default.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/** @brief 比较热缓存文件传输和相同整数依赖链，计时不含持久化。 */
namespace {
using clock_type = std::chrono::steady_clock;

/** @brief 输出每次操作的耗时、吞吐量与工作量校验和。 */
void report(std::string_view scenario, clock_type::time_point start,
            std::size_t operations, std::size_t bytes, std::uint64_t checksum = 0) {
  const double elapsed = std::chrono::duration<double>(clock_type::now() - start).count();
  std::cout << scenario << ',' << std::fixed << std::setprecision(3)
            << elapsed * 1e9 / static_cast<double>(operations) << ','
            << static_cast<double>(operations) / elapsed << ','
            << static_cast<double>(bytes) / elapsed / 1048576.0 << ','
            << operations << ',' << bytes << ',' << checksum << '\n';
}

/** @brief 预热普通文件后测量指定块大小的完整读写。 */
faio::task<void> file_case(const std::filesystem::path& path, std::size_t block_size,
                           std::size_t operations) {
  auto file = co_await faio::fs::File::open(
      path, faio::fs::OpenOptions{}.read().write().create().truncate());
  if (!file)
    throw std::runtime_error(std::string{file.error().message()});

  std::vector<char> buffer(block_size, 'f');
  for (std::size_t i = 0; i < operations; ++i) {
    const auto written = co_await file->write_all(std::span<const char>{buffer});
    if (!written)
      throw std::runtime_error(std::string{written.error().message()});
  }

  const auto warmed = co_await file->sync_data();
  if (!warmed)
    throw std::runtime_error(std::string{warmed.error().message()});
  const auto rewound = co_await file->seek(0);
  if (!rewound)
    throw std::runtime_error(std::string{rewound.error().message()});

  const auto start_write = clock_type::now();
  for (std::size_t i = 0; i < operations; ++i) {
    const auto written = co_await file->write_all(std::span<const char>{buffer});
    if (!written)
      throw std::runtime_error(std::string{written.error().message()});
  }
  const auto flushed = co_await file->flush();
  if (!flushed)
    throw std::runtime_error(std::string{flushed.error().message()});
  report("file_write_all_" + std::to_string(block_size), start_write, operations,
         block_size * operations);

  const auto synced = co_await file->sync_data();
  if (!synced)
    throw std::runtime_error(std::string{synced.error().message()});
  const auto rerewound = co_await file->seek(0);
  if (!rerewound)
    throw std::runtime_error(std::string{rerewound.error().message()});

  std::fill(buffer.begin(), buffer.end(), '\0');
  const auto start_read = clock_type::now();
  std::uint64_t checksum = 0;
  for (std::size_t i = 0; i < operations; ++i) {
    const auto count = co_await file->read_exact(std::span<char>{buffer});
    if (!count)
      throw std::runtime_error(std::string{count.error().message()});
    if (*count != block_size || buffer.front() != 'f' || buffer.back() != 'f')
      throw std::runtime_error("file data mismatch");
    checksum += static_cast<unsigned char>(buffer.front());
  }
  report("file_read_exact_" + std::to_string(block_size), start_read, operations,
         block_size * operations, checksum);

  const auto closed = co_await file->close();
  if (!closed)
    throw std::runtime_error(std::string{closed.error().message()});
}

/** @brief 执行固定轮数的整数依赖链并返回校验值。 */
std::uint64_t cpu_work(std::uint64_t value, std::size_t rounds) noexcept {
  for (std::size_t i = 0; i < rounds; ++i) {
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    value *= 2685821657736338717ULL;
  }
  return value;
}

/** @brief 在协程中执行固定整数计算负载。 */
faio::task<std::uint64_t> cpu_task(std::uint64_t seed, std::size_t rounds) {
  co_return cpu_work(seed, rounds);
}

/** @brief 比较普通协程与阻塞池提交 CPU 负载的开销。 */
faio::task<void> cpu_case(std::size_t jobs, std::size_t rounds, bool blocking) {
  std::vector<faio::join_handle<std::uint64_t>> handles;
  handles.reserve(jobs);
  const auto start = clock_type::now();
  for (std::size_t i = 0; i < jobs; ++i) {
    if (blocking)
      handles.push_back(faio::spawn_blocking([i, rounds] { return cpu_work(i + 1, rounds); }));
    else
      handles.push_back(faio::spawn(cpu_task(i + 1, rounds)));
  }
  std::uint64_t checksum = 0;
  for (auto& handle : handles)
    checksum ^= co_await handle;
  report(std::string{blocking ? "cpu_spawn_blocking_" : "cpu_spawn_"}
             + std::to_string(rounds), start, jobs, 0, checksum);
}

/** @brief 运行选定的文件 I/O 和 CPU 负载场景。 */
faio::task<void> suite(const std::filesystem::path& directory, std::size_t n,
                       std::string_view selection) {
  if (selection != "cpu") {
    for (const auto block : {4096uz, 65536uz, 8388608uz}) {
      const auto count = std::max<std::size_t>(8, n * 4096 / block);
      const auto path = directory / ("faio-perf-" + std::to_string(block) + ".data");
      co_await file_case(path, block, count);
      std::filesystem::remove(path);
    }
  }
  if (selection != "io") {
    co_await cpu_case(128, 20000, false);
    co_await cpu_case(128, 20000, true);
    co_await cpu_case(128, 200000, false);
    co_await cpu_case(128, 200000, true);
  }
}
}

/** @brief 解析参数并运行文件 I/O 及 CPU 负载基准。 */
int main(int argc, char** argv) {
  try {
    if (argc < 2)
      throw std::invalid_argument("usage: io_cpu_benchmark DIRECTORY [4k-operations=2048] [workers=4] [all|io|cpu]");
    const std::filesystem::path directory{argv[1]};
    if (!std::filesystem::is_directory(directory))
      throw std::invalid_argument("directory must already exist");
    const auto n = argc > 2 ? std::stoull(argv[2]) : 2048uz;
    const auto workers = argc > 3 ? std::stoull(argv[3]) : 4uz;
    const std::string_view selection = argc > 4 ? argv[4] : "all";
    if (!n || !workers || (selection != "all" && selection != "io" && selection != "cpu"))
      throw std::invalid_argument("invalid count, workers or suite");
    faio::log::logger()->set_level(spdlog::level::off);
    auto config = faio::config_builder{}.set_num_workers(workers)
                      .set_max_blocking_threads(workers).set_filesystem_threads(workers).build();
    faio::runtime::detail::runtime_context context{config};
    std::cout << "scenario,ns_per_op,ops_per_sec,mib_per_sec,operations,bytes,checksum\n";
    context.block_on(suite(directory, n, selection));
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
