/**
 * @file faio/detail/net/platform/unix/socket_association.hpp
 * @brief 网络地址、socket 与传输协议：socket 所属后端关联。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
#include "faio/detail/io/core/context.hpp"
#include "faio/detail/net/platform/select.hpp"
#include <utility>

namespace faio::net::detail {
/** @brief Unix 原生 socket 没有不可迁移的完成端口关联；不增加拥有者大小。 */
struct native_socket_association {
  native_socket_association() noexcept = default;

  explicit native_socket_association(io::io_context) noexcept {}

  expected<void> validate(const io::io_context&) const noexcept { return {}; }

  void forget(native_socket_type) noexcept {}

  void release_imported() noexcept {}

  static native_socket_association capture(const io::detail::resource_ptr&) noexcept { return {}; }

  /// @brief 撤销原生资源登记，迟到事件仍须由上层按资源身份过滤。
  template <class F>
  static auto detach(const io::detail::resource_ptr&, F&& fallback) {
    return std::forward<F>(fallback)();
  }
};
}
