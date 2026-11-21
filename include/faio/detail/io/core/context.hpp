/**
 * @file faio/detail/io/core/context.hpp
 * @brief 上下文与服务入口。
 */
#pragma once
#include "faio/detail/blocking/service.hpp"
#include "faio/detail/common/move_only_function.hpp"
#include <utility>
#include "faio/detail/io/protocol/backend.hpp"
#include "faio/detail/io/core/fwd.hpp"
#include "faio/detail/io/core/types.hpp"
#include <memory>
#include <stop_token>

namespace faio::io {
/** @brief 可复制的 IO 服务租约；存活不等于 runtime 仍接受新请求。 */
class io_context {
 public:
  /** @brief 构造 I/O 上下文租约。 */
  io_context() noexcept = default;

  /** @brief 构造 I/O 上下文租约。 */
  explicit io_context(std::shared_ptr<detail::io_domain> domain) noexcept
      : domain_(std::move(domain)) {}

  /** @brief 取得当前线程绑定的 I/O 上下文。 */
  static io_context current();

  /** @brief 判断是否持有有效的 I/O 域租约。 */
  bool valid() const noexcept { return !!domain_; }

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return valid(); }

  /** @brief 判断停止请求是否已经生效。 */
  bool stopped() const noexcept;

  /** @brief 取得协作停止令牌。 */
  std::stop_token stop_token() const noexcept;

  /** @brief 读取累计运行统计。 */
  detail::backend_statistics statistics() const noexcept;

  /** @brief 取得阻塞任务执行服务。 */
  blocking::service_ref blocking() const noexcept;

  /** @brief 取得专门执行关闭任务的清理服务。 */
  blocking::service_ref cleanup() const noexcept;

  /** @brief 取得域名解析服务。 */
  blocking::service_ref resolver() const noexcept;

  /**
   * @brief 为尚未注册的新连接选择同一运行时的目标分片，保留现有资源的归属。
   * @details 独立引擎未共享 placement 服务时返回自身；全部分片停止时返回空上下文。
   */
  io_context balanced_context() const noexcept;

  /** @brief 清理使用独立预留通道；普通任务队列满时，也不会在 worker 上执行 close。 */
  void defer_cleanup(::faio::move_only_function<void()> cleanup) const noexcept;

  /** @brief 注册停机清理；控制面调用，回调不应阻塞驱动线程。 */
  std::uint64_t register_shutdown_cleanup(::faio::move_only_function<void()> cleanup) const;

  /** @brief 撤销运行时关闭清理登记。 */
  void unregister_shutdown_cleanup(std::uint64_t token) const noexcept;

  /** @brief 取得 I/O 归属域。 */
  const std::shared_ptr<detail::io_domain>& domain() const noexcept { return domain_; }

  /** @brief 比较两个对象的值。 */
  friend bool operator==(const io_context&, const io_context&) = default;

 private:
  std::shared_ptr<detail::io_domain>
      domain_; /**< 共享持有所属 I/O 域，使其在取消回调和完成发布期间保持有效。 */
};

/** @brief 取得该对象关联的 I/O 上下文租约。 */
inline io_context current_context() {
  return io_context::current();
}
}  // namespace faio::io
