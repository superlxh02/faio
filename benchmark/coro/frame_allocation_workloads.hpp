/**
 * @file benchmark/coro/frame_allocation_workloads.hpp
 * @brief 协程帧分配测试负载。
 */
#pragma once

#include "faio/detail/coroutine/task.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

/** @brief 工厂位于独立翻译单元，并禁用 LTO 以确保实际分配独立协程帧。 */
faio::task<std::uint64_t> make_small_benchmark_frame();
/** @brief 创建返回 512 字节数组的协程帧负载。 */
faio::task<std::array<std::byte, 512>> make_medium_benchmark_frame();
/** @brief 创建返回 4096 字节数组的协程帧负载。 */
faio::task<std::array<std::byte, 4096>> make_large_benchmark_frame();
