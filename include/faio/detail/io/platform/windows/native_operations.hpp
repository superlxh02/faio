/**
 * @file faio/detail/io/platform/windows/native_operations.hpp
 * @brief 跨平台异步 IO：原生操作适配。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#include "faio/detail/io/platform/native_types.hpp"
#include "faio/detail/io/platform/windows/error.hpp"
#include "faio/detail/io/protocol/backend.hpp"

namespace faio::io::detail::native_operations {
/** @brief 单次关闭；EINTR 后句柄可能已复用，不能重试。 */
inline expected<std::int64_t> close_file_cleanup(native_descriptor fd) noexcept {
  if (!::CloseHandle(reinterpret_cast<HANDLE>(fd)))
    return std::unexpected{windows::make_windows_error(::GetLastError())};
  return std::int64_t{0};
}

/// @brief 关闭尚未绑定 IO 域的原生资源，确保导入失败路径不泄漏句柄。
template <class Resource>
void close_unbound_resource(Resource& resource) noexcept {
  const auto fd = resource.handle.exchange(-1);
  if (fd < 0)
    return;
  if (resource.native_kind == native_handle_kind::windows_handle)
    (void)::CloseHandle(reinterpret_cast<HANDLE>(fd));
  else
    (void)::closesocket(static_cast<SOCKET>(fd));
}

/// @brief 初始化资源的原生种类及平台属性，后续热路径使用缓存值。
template <class Resource>
void initialize_resource_kind(Resource& resource, bool regular) noexcept {
  resource.native_kind =
      regular ? native_handle_kind::windows_handle : native_handle_kind::windows_socket;
}

/// @brief 配置原生描述符的非阻塞状态，错误按平台原生约定转换。
inline expected<void> set_nonblocking(native_descriptor fd, bool enabled) noexcept {
  if (!enabled)
    return std::unexpected{make_error(EINVAL)};
  u_long mode = 1;
  if (::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == SOCKET_ERROR)
    return std::unexpected{windows::make_winsock_error(::WSAGetLastError())};
  return {};
}

/// @brief 配置原生描述符的非阻塞状态，错误按平台原生约定转换。
inline expected<bool> nonblocking(native_descriptor) noexcept {
  return true;
}
}
