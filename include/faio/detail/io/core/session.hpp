/**
 * @file faio/detail/io/core/session.hpp
 * @brief 跨平台异步 IO：驱动会话与重入保护。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/io/core/fwd.hpp"
#include <utility>

namespace faio::io::detail {
inline thread_local io_domain* current_domain{};
/// @brief 当前线程的 CQ 驱动 session；与业务 coroutine 的 current_domain
/// 分开保存。
inline thread_local io_domain* current_driver_domain{};

/** @brief 只维护 CQ 消费者的线程内防重入纪律，不拥有请求或原生后端。
 * @details 已有 session 时调用者应直接跳过新 session；避免同线程再次 try_lock
 *          已持有的非递归 driver mutex。析构恢复原值，不改变业务资源归属。
 */
class driver_session_guard {
 public:
  explicit driver_session_guard(io_domain* domain) noexcept
      : previous_(std::exchange(current_driver_domain, domain)) {}

  driver_session_guard(const driver_session_guard&) = delete;

  driver_session_guard& operator=(const driver_session_guard&) = delete;

  ~driver_session_guard() {
    /// @brief 所有返回路径都撤销本次线程内 session。
    current_driver_domain = previous_;
  }

 private:
  io_domain* previous_;  ///< 仅借用地址；真正域租约由 driver/caller 生命周期持有。
};

}
