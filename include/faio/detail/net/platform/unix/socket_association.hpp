/**
 * @file faio/detail/net/platform/unix/socket_association.hpp
 * @brief socket 所属后端关联。
 */
#pragma once
#include "faio/detail/io/core/context.hpp"
#include "faio/detail/net/platform/select.hpp"
#include <utility>

namespace faio::net::detail {
/**  @brief Unix 原生 socket 没有不可迁移的完成端口关联；不增加拥有者大小。 */
struct native_socket_association {
  /** @brief 构造无需完成端口关联的 Unix 适配对象。 */
  native_socket_association() noexcept = default;

  /** @brief 构造无需完成端口关联的 Unix 适配对象。 */
  explicit native_socket_association(io::io_context) noexcept {}

  /** @brief Unix socket 无需完成端口归属校验。 */
  expected<void> validate(const io::io_context&) const noexcept { return {}; }

  /** @brief Unix socket 无需额外清理完成端口关联。 */
  void forget(native_socket_type) noexcept {}

  /** @brief Unix 导入不保存额外完成端口登记。 */
  void release_imported() noexcept {}

  /** @brief 返回无需完成端口关联的 Unix 适配值。 */
  static native_socket_association capture(const io::detail::resource_ptr&) noexcept { return {}; }

  /** @brief 将句柄转出交给调用方提供的 Unix 移交路径。 */
  template <class F>
  static auto detach(const io::detail::resource_ptr&, F&& fallback) {
    return std::forward<F>(fallback)();
  }
};
}  // namespace faio::net::detail
