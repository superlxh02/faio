/**
 * @file benchmark/coro/frame_allocation_benchmark.cpp
 * @brief 协程帧分配性能基准。
 */
#include "frame_allocation_workloads.hpp"
#include "faio/detail/coroutine/memory/frame_allocator.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
/** @brief 批量创建并销毁独立工厂生成的协程帧。 */
template <class Factory>
void measure(std::string_view name, Factory factory, std::size_t batch, std::size_t count) {
  using task_type = decltype(factory());
  std::array<task_type, 64> tasks;
  const auto run = [&](std::size_t iterations) {
    for (std::size_t i = 0; i < iterations; ++i) {
      for (std::size_t j = 0; j < batch; ++j)
        tasks[j] = factory();
      for (std::size_t j = 0; j < batch; ++j)
        tasks[j] = {};
    }
  };
  run(10000 / batch);
  const auto iterations = count / batch;
  const auto start = std::chrono::steady_clock::now();
  run(iterations);
  const auto nanoseconds = std::chrono::duration<double, std::nano>(
      std::chrono::steady_clock::now() - start).count();
  std::cout << name << ',' << batch << ',' << iterations * batch << ','
            << std::fixed << std::setprecision(3)
            << nanoseconds / static_cast<double>(iterations * batch) << '\n';
}
}

/** @brief 解析批量大小并测量三种协程帧的分配销毁成本。 */
int main(int argc, char** argv) {
  const auto count = argc > 1 ? std::stoull(argv[1]) : 1000000ULL;
  if (count < 64)
    throw std::invalid_argument("count must be at least 64");
  faio::detail::coroutine_frame_cache_scope scope;
  std::cout << "scenario,batch,operations,ns_per_frame\n";
  for (const auto batch : {std::size_t{1}, std::size_t{64}}) {
    measure("small_lazy_frame", make_small_benchmark_frame, batch, count);
    measure("medium_lazy_frame", make_medium_benchmark_frame, batch, count);
    measure("large_lazy_frame", make_large_benchmark_frame, batch, count);
  }
}
