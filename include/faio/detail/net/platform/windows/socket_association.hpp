/**
 * @file faio/detail/net/platform/windows/socket_association.hpp
 * @brief 网络地址、socket 与传输协议：socket 所属后端关联。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
#include "faio/detail/io/core/context.hpp"
#include "faio/detail/net/platform/select.hpp"
#include <utility>

namespace faio::net::detail {
/** @brief 导出的 SOCKET 保留原完成端口租约，阻止带旧关联的句柄跨域导入。 */
class native_socket_association {
 public:
  native_socket_association() noexcept = default;

  explicit native_socket_association(io::io_context context) noexcept
      : context_(std::move(context)) {}

  expected<void> validate(const io::io_context& context) const noexcept {
    if (context_) {
      if (context_.stopped())
        return std::unexpected{make_error(ECANCELED)};
      if (context_.domain() != context.domain())
        return std::unexpected{make_error(EXDEV)};
    }
    return {};
  }

  void forget(native_socket_type descriptor) noexcept {
    if (context_ && descriptor >= 0)
      context_.domain()->forget_native_handle(descriptor,
                                              io::detail::native_handle_kind::windows_socket);
    context_ = {};
  }

  void release_imported() noexcept { context_ = {}; }

  static native_socket_association capture(const io::detail::resource_ptr& resource) noexcept {
    return native_socket_association{resource ? io::io_context{resource->owner} : io::io_context{}};
  }

  /// @brief 撤销原生资源登记，迟到事件仍须由上层按资源身份过滤。
  template <class F>
  static auto detach(const io::detail::resource_ptr& resource, F&& fallback) {
    if (resource && resource->owner)
      return resource->owner->detach(*resource, true);
    return std::forward<F>(fallback)();
  }

 private:
  io::io_context context_;  ///< 持有所属 IO 上下文租约，用于资源归属、服务选择与安全排空。
};
}
