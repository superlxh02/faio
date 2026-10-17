/**
 * @file faio/detail/io/core/context.hpp
 * @brief 跨平台异步 IO：上下文与服务入口。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/execution/blocking_executor.hpp"
#include "faio/detail/io/protocol/backend.hpp"
#include "faio/detail/io/core/fwd.hpp"
#include "faio/detail/io/core/types.hpp"
#include <functional>
#include <memory>
#include <stop_token>

namespace faio::io {
/// @brief 可复制的 IO 服务租约；存活不等于 runtime 仍接受新请求。
class io_context {
 public:
  io_context() noexcept = default;

  explicit io_context(std::shared_ptr<detail::io_domain> domain) noexcept
      : domain_(std::move(domain)) {}

  static io_context current();

  /// @brief 检查当前包装是否持有有效资源身份。
  bool valid() const noexcept { return !!domain_; }

  explicit operator bool() const noexcept { return valid(); }

  bool stopped() const noexcept;

  std::stop_token stop_token() const noexcept;

  detail::backend_statistics statistics() const noexcept;

  execution::blocking_executor_ref blocking() const noexcept;

  execution::blocking_executor_ref cleanup() const noexcept;

  execution::blocking_executor_ref resolver() const noexcept;

  /** @brief 为尚未注册的新连接选择同一 runtime 的目标
   * shard；现有资源归属不受影响。
   * @details 独立引擎没有共享 placement 服务时返回自身；全组停止时返回空
   * context。
   */
  io_context balanced_context() const noexcept;

  /// @brief 清理走独立保留通道，普通任务队列满也不在 worker 执行 close。
  void defer_cleanup(::faio::move_only_function<void()> cleanup) const noexcept;

  /// @brief 注册停机清理；控制面调用，回调不应阻塞驱动线程。
  std::uint64_t register_shutdown_cleanup(::faio::move_only_function<void()> cleanup) const;

  void unregister_shutdown_cleanup(std::uint64_t token) const noexcept;

  const std::shared_ptr<detail::io_domain>& domain() const noexcept { return domain_; }

  friend bool operator==(const io_context&, const io_context&) = default;

 private:
  std::shared_ptr<detail::io_domain> domain_;  ///< 共享持有所属 IO 域，覆盖取消回调与完成发布。
};

/// @brief 取得当前对象关联的 IO 上下文租约。
inline io_context current_context() {
  return io_context::current();
}
}
