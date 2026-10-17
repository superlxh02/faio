/**
 * @file faio/detail/io/core/types.hpp
 * @brief 跨平台异步 IO：公共协议类型。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/io/platform/native_types.hpp"
#include <cstddef>
#include <cstdint>

namespace faio::io {
/** @brief 就绪观察方向；观察者不占用实际读写执行权。 */
enum class Interest : std::uint32_t { none = 0, readable = 1, writable = 2, read_write = 3 };

inline constexpr Interest operator|(Interest a, Interest b) noexcept {
  return static_cast<Interest>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

/** @brief 就绪快照；允许假就绪，真实 syscall 的 EAGAIN 才清除缓存。 */
struct Ready {
  std::uint32_t flags{};  ///< 本次原生操作的标志位，按对应系统调用语义解释。
  std::uint64_t generation{};  ///< 本次完成观察到的资源代际，供 readiness guard 条件清除。

  bool is_readable() const noexcept { return flags & 1; }

  bool is_writable() const noexcept { return flags & 2; }

  bool is_error() const noexcept { return flags & 4; }

  /// @brief 读方向已收到 EOF 提示；真实 read 仍可能先返回尚未消费的数据。
  bool is_read_closed() const noexcept { return flags & 8; }

  /// @brief 写方向已关闭；本地写半关闭不表示仍可接收响应的读方向关闭。
  bool is_write_closed() const noexcept { return flags & 16; }
};

enum class ShutdownBehavior { Read = SHUT_RD, Write = SHUT_WR, Both = SHUT_RDWR };

enum class cancel_reason : std::uint8_t { user, deadline, close, shutdown };

enum class shutdown_policy : std::uint8_t { drain, cancel_all };

/** @brief 槽位加代际 token；后端从不持有 awaiter 地址。 */
struct operation_token {
  std::uint64_t value{};  ///< 持有当前结果值或快照，读取前须确认操作已完成。

  friend bool operator==(operation_token, operation_token) = default;
};

struct drive_budget {
  std::size_t max_events{256},  ///< 本次驱动允许消费的后端事件数量上限。
      max_completions{256};  ///< 本次驱动允许向协程交付的完成数量上限。
};

struct drive_result {
  std::size_t completions{};  ///< 本轮已经发布给等待者的完成数量。
  bool progressed{},  ///< 本轮是否推进了 IO、定时器或完成交付。
      more_work{};  ///< 是否仍有受预算限制而留待后续驱动的工作。
  int fatal_error{};  ///< 驱动发现的永久错误码，零表示没有永久故障。
};

}
