/**
 * @file faio/detail/net/platform/windows/socket_association.hpp
 * @brief socket 所属后端关联。
 */
#pragma once
#include "faio/detail/io/core/context.hpp"
#include "faio/detail/io/core/domain.hpp"
#include "faio/detail/net/platform/select.hpp"
#include <utility>

namespace faio::net::detail {
/**  @brief 导出的 SOCKET 保留原完成端口租约，阻止带旧关联的句柄跨域导入。 */
class native_socket_association {
 public:
  /** @brief 保存原生 socket 的 IO 后端关联信息。 */
  native_socket_association() noexcept = default;

  /** @brief 保存原生 socket 的 IO 后端关联信息。 */
  explicit native_socket_association(io::io_context context) noexcept
      : context_(std::move(context)) {}

  /** @brief 检查原生 socket 的后端关联是否兼容目标 IO 上下文。 */
  expected<void> validate(const io::io_context& context) const noexcept {
    if (context_) {
      if (context_.stopped())
        return std::unexpected{make_error(ECANCELED)};
      if (context_.domain() != context.domain())
        return std::unexpected{make_error(EXDEV)};
    }
    return {};
  }

  /** @brief 撤销当前原生 socket 的关联记录。 */
  void forget(native_socket_type descriptor) noexcept {
    if (context_ && descriptor >= 0)
      context_.domain()->forget_native_handle(descriptor,
                                              io::detail::native_handle_kind::WINDOWS_SOCKET);
    context_ = {};
  }

  /** @brief 交出导入资源的登记责任，避免后续重复撤销。 */
  void release_imported() noexcept { context_ = {}; }

  /** @brief 取得底层资源的原生 socket 关联状态。 */
  static native_socket_association capture(const io::detail::resource_ptr& resource) noexcept {
    return native_socket_association{resource ? io::io_context{resource->owner} : io::io_context{}};
  }

  /** @brief 撤销原生资源登记，迟到事件仍须由上层按资源身份过滤。 */
  template <class F>
  static auto detach(const io::detail::resource_ptr& resource, F&& fallback) {
    if (resource && resource->owner)
      return resource->owner->detach(*resource, true);
    return std::forward<F>(fallback)();
  }

 private:
  io::io_context context_; /**< 持有所属 IO 上下文租约，用于资源归属、服务选择与安全排空。 */
};
}  // namespace faio::net::detail
