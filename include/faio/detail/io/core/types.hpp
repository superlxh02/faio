/**
 * @file faio/detail/io/core/types.hpp
 * @brief 公共协议类型。
 */
#pragma once
#include "faio/detail/io/platform/native_types.hpp"
#include <cstddef>
#include <cstdint>

namespace faio::io {
/**  @brief 就绪观察方向；观察者不占用实际读写执行权。 */
enum class Interest : std::uint32_t { none = 0, readable = 1, writable = 2, read_write = 3 };

/** @brief 组合标志选项。 */
inline constexpr Interest operator|(Interest a, Interest b) noexcept {
  return static_cast<Interest>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

/**  @brief 就绪快照；允许假就绪，真实 syscall 的 EAGAIN 才清除缓存。 */
struct Ready {
  std::uint32_t flags{};      /**< 本次观察到的就绪标志位。 */
  std::uint64_t generation{}; /**< 本次完成观察到的资源代际，供 readiness guard 按条件清除。 */

  /** @brief 判断当前资源是否可读。 */
  bool is_readable() const noexcept { return flags & 1; }

  /** @brief 判断当前资源是否可写。 */
  bool is_writable() const noexcept { return flags & 2; }

  /** @brief 判断操作结果是否为错误。 */
  bool is_error() const noexcept { return flags & 4; }

  /** @brief 读方向已收到 EOF 提示；真实 read 仍可能先返回尚未消费的数据。 */
  bool is_read_closed() const noexcept { return flags & 8; }

  /** @brief 写方向已关闭；本地写半关闭不表示仍可接收响应的读方向关闭。 */
  bool is_write_closed() const noexcept { return flags & 16; }
};

enum class ShutdownBehavior { Read = SHUT_RD, Write = SHUT_WR, Both = SHUT_RDWR };

enum class cancel_reason : std::uint8_t { user, deadline, close, shutdown };

enum class shutdown_policy : std::uint8_t { drain, cancel_all };

/**  @brief 由槽位和代际组成的操作令牌；后端不持有 awaiter 地址。 */
struct operation_token {
  std::uint64_t value{}; /**< 包含槽位和代际的令牌值。 */

  /** @brief 比较两个对象的值。 */
  friend bool operator==(operation_token, operation_token) = default;
};

/** @brief 单次 I/O 驱动的工作预算。 */
struct drive_budget {
  std::size_t max_events{256}, max_completions{256}; /**< 本次驱动允许向协程交付的完成数量上限。 */
};

/** @brief 单次 I/O 驱动的进展与统计。 */
struct drive_result {
  std::size_t completions{};      /**< 本轮已经发布给等待者的完成数量。 */
  bool progressed{}, more_work{}; /**< 是否仍有受预算限制而留待后续驱动的工作。 */
  int fatal_error{};              /**< 驱动发现的永久错误码，零表示没有永久故障。 */
};

}  // namespace faio::io
