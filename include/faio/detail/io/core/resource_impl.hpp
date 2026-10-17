/**
 * @file faio/detail/io/core/resource_impl.hpp
 * @brief 跨平台异步 IO：资源释放实现。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/io/core/domain.hpp"
#include "faio/detail/io/platform/native_operations.hpp"

namespace faio::io::detail {
/// @brief 按轮转策略选择可接纳资源的 IO 域。
inline std::shared_ptr<io_domain> io_placement_group::select() noexcept {
  /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
  std::lock_guard lock(mutex_);
  for (std::size_t inspected = 0; inspected < domains_.size(); ++inspected) {
    const auto index = next_++ % domains_.size();
    /// @brief 返回拥有型租约，adopt 完成前目标 domain 不会析构。
    if (auto domain = domains_[index].lock(); domain && !domain->stopped())
      return domain;
  }
  /// @brief 全组已停止/过期，调用方的原生 guard 保留失败关闭责任。
  return {};
}

/// @brief 按已登记原生句柄查找资源，不创建新的资源身份。
inline resource_ptr io_placement_group::find_resource(native_descriptor fd,
                                                      const io_domain* skipped) noexcept {
  /// @brief 域数量在构造后固定；每个 weak_ptr 的读取仍必须在组锁内串行。
  for (std::size_t index = 0; index < domains_.size(); ++index) {
    /// @brief 临时拥有域，离开组锁后仍可安全读取其资源表。
    std::shared_ptr<io_domain> domain;
    {
      /// @brief 组锁只负责提升弱租约，不进入任何域方法。
      std::lock_guard lock(mutex_);
      domain = domains_[index].lock();
    }
    /// @brief 过期域与已经查询过的本域没有额外锁或注册副作用。
    if (!domain || domain.get() == skipped)
      continue;
    /// @brief 此时只持有资源租约，不保留组锁或域锁。
    if (auto resource = domain->find_registered_resource(fd))
      return resource;
  }
  /// @brief 完全未知的 raw fd 继续沿调用方域的现有借用注册路径处理。
  return {};
}

inline resource_state::~resource_state() {
  if (owner)
    owner->release_resource(*this);
  else if (owns_handle)
    native_operations::close_unbound_resource(*this);
}

/// @brief 请求写半关闭，等待当前写入及组合写预留解除后再发出关闭动作。
inline void resource_state::request_shutdown_write() noexcept {
  if (owner)
    owner->request_shutdown_write(*this);
}

inline resource_ptr adopt_resource(const io_context& context,
                                   native_descriptor fd,
                                   bool owns = true,
                                   bool regular = false) {
  if (context)
    return context.domain()->adopt(fd, owns, regular);
  auto resource = std::make_shared<resource_state>();
  resource->handle = fd;
  resource->owns_handle = owns;
  resource->regular_file = regular;
  native_operations::initialize_resource_kind(*resource, regular);
  return resource;
}

/// @brief 为本次请求保存共享资源租约，固定句柄身份及所属 IO 域。
template <class F>
auto with_resource(const resource_ptr& resource,
                   Interest interest,
                   F&& function,
                   void* reservation = nullptr) -> std::invoke_result_t<F> {
  using result_type = std::invoke_result_t<F>;
  if (!resource)
    return result_type{std::unexpected{make_error(EBADF)}};
  if (resource->owner)
    return resource->owner->with_resource(
        resource, interest, std::forward<F>(function), reservation);
  if (resource->fd() < 0)
    return result_type{std::unexpected{make_error(EBADF)}};
  return std::invoke(std::forward<F>(function));
}

}
