/**
 * @file faio/detail/runtime/common/constants.hpp
 * @brief 调度队列与时间轮的固定容量，不引入运行时配置或格式化实现。
 */
#pragma once
#include <cstddef>

namespace faio::runtime::detail {
static inline constexpr std::size_t MAX_LEVEL{6uz};
static inline constexpr std::size_t SLOT_SIZE{64uz};
/** @brief 槽位数量以 2 为底的对数。 */
static inline constexpr std::size_t SLOT_SHIFT{6uz};
/** @brief 槽位索引的位掩码。 */
static inline constexpr std::size_t SLOT_MASK{SLOT_SIZE - 1uz};
static inline constexpr std::size_t LOCAL_QUEUE_CAPACITY{256uz};
}  // namespace faio::runtime::detail
