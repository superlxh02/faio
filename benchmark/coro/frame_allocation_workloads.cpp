/**
 * @file benchmark/coro/frame_allocation_workloads.cpp
 * @brief 协程帧分配测试负载。
 */
#include "frame_allocation_workloads.hpp"

/** @brief 创建返回整数的小型协程帧负载。 */
faio::task<std::uint64_t> make_small_benchmark_frame() { co_return 42; }

/** @brief 创建返回 512 字节数组的协程帧负载。 */
faio::task<std::array<std::byte, 512>> make_medium_benchmark_frame() {
  co_return std::array<std::byte, 512>{};
}

/** @brief 创建返回 4096 字节数组的协程帧负载。 */
faio::task<std::array<std::byte, 4096>> make_large_benchmark_frame() {
  co_return std::array<std::byte, 4096>{};
}
