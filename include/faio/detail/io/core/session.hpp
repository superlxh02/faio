/**
 * @file faio/detail/io/core/session.hpp
 * @brief 驱动会话与重入保护。
 */
#pragma once
#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/io/core/fwd.hpp"
#include <utility>

namespace faio::io::detail {
/**
 * @brief 防止 CQ 消费者在同一线程内重入，不持有请求或原生后端的所有权。
 * @details 已存在会话时，调用方应跳过新会话，避免在同一线程内再次 try_lock
 * 已持有的非递归驱动锁。驱动域与业务默认域分别保存于线程上下文；
 * 析构仅恢复驱动会话，不改变业务资源的归属。
 */
class driver_session_guard {
 public:
  /** @brief 构造 I/O 驱动会话保护。 */
  explicit driver_session_guard(io_domain* domain) noexcept
      : previous_(
            std::exchange(::faio::detail::current_thread_context().io.driver_domain, domain)) {}

  /** @brief 禁止复制构造 I/O 驱动会话保护。 */
  driver_session_guard(const driver_session_guard&) = delete;

  /** @brief 禁止复制赋值。 */
  driver_session_guard& operator=(const driver_session_guard&) = delete;

  /** @brief 销毁 I/O 驱动会话保护。 */
  ~driver_session_guard() {
    if (auto* context = ::faio::detail::try_current_thread_context())
      context->io.driver_domain = previous_;
  }

 private:
  io_domain* previous_; /**< 仅借用域地址；实际域租约须由驱动器或调用方在借用期间保持有效。 */
};

}  // namespace faio::io::detail
