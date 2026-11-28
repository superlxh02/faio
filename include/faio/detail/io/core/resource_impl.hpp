/**
 * @file faio/detail/io/core/resource_impl.hpp
 * @brief 资源释放实现。
 */
#pragma once
#include "faio/detail/io/core/domain.hpp"
#include "faio/detail/io/platform/native_operations.hpp"

namespace faio::io::detail {
/** @brief 按轮转策略选择可接纳资源的 IO 域。 */
inline std::shared_ptr<io_domain> io_placement_group::select() noexcept {
  std::lock_guard lock(mutex_);
  for (std::size_t inspected = 0; inspected < domains_.size(); ++inspected) {
    const auto index = next_++ % domains_.size();
    if (auto domain = domains_[index].lock(); domain && !domain->stopped())
      return domain;
  }
  return {};
}

/** @brief 按已登记原生句柄查找资源，不创建新的资源身份。 */
inline resource_ptr io_placement_group::find_resource(native_descriptor fd,
                                                      const io_domain* skipped) noexcept {
  for (std::size_t index = 0; index < domains_.size(); ++index) {
    std::shared_ptr<io_domain> domain;
    {
      std::lock_guard lock(mutex_);
      domain = domains_[index].lock();
    }
    if (!domain || domain.get() == skipped)
      continue;
    if (auto resource = domain->find_registered_resource(fd))
      return resource;
  }
  return {};
}

/** @brief 销毁原生 I/O 资源的共享状态。 */
inline resource_state::~resource_state() {
  if (owner)
    owner->release_resource(*this);
  else if (owns_handle)
    native_operations::close_unbound_resource(*this);
}

/** @brief 请求写半关闭，等待当前写入及组合写预留解除后再发出关闭动作。 */
inline void resource_state::request_shutdown_write() noexcept {
  if (owner)
    owner->request_shutdown_write(*this);
}

/** @brief 接管原生资源的关闭责任。 */
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

/** @brief 为本次请求保存共享资源租约，保持句柄身份及所属 I/O 域不变。 */
template <class F>
auto with_resource(const resource_ptr& resource,
                   interest interest,
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

}  // namespace faio::io::detail
