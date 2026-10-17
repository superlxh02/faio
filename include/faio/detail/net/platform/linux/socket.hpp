/**
 * @file faio/detail/net/platform/linux/socket.hpp
 * @brief 网络地址、socket 与传输协议：socket 资源封装。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
#include "faio/detail/net/platform/unix/socket.hpp"
#include <cstddef>
#include <cstring>
#include <string_view>

namespace faio::net::detail {
/// @brief 为导入或新建 socket 设置平台必要的非阻塞及继承属性。
inline expected<void> prepare_native_socket(native_socket_type descriptor) noexcept {
  return prepare_socket_descriptor(descriptor);
}

/// @brief 创建平台 socket 并在失败时归还已经取得的句柄。
inline expected<native_socket_type> create_native_socket(int domain,
                                                         int type,
                                                         int protocol) noexcept {
  const int descriptor = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
  if (descriptor < 0)
    return std::unexpected{socket_error()};
  return descriptor;
}

/// @brief 尝试接受一个原生连接，结果保留暂不可用与永久错误的区别。
inline native_socket_type accept_native_socket(native_socket_type descriptor,
                                               sockaddr* address,
                                               socklen_t* length) noexcept {
  return ::accept4(descriptor, address, length, SOCK_NONBLOCK | SOCK_CLOEXEC);
}

template <class Address>
inline void set_native_address_length(Address&) noexcept {}

inline void set_native_address_length(sockaddr_un&, socklen_t) noexcept {}

inline expected<socklen_t> make_abstract_address(sockaddr_un& address, std::string_view name) {
  if (name.size() > sizeof(address.sun_path) - 1)
    return std::unexpected{make_error(ENAMETOOLONG)};
  std::memcpy(address.sun_path + 1, name.data(), name.size());
  return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
}

template <class Credentials>
inline expected<Credentials> socket_peer_credentials(native_socket_type descriptor) noexcept {
  struct ucred credentials{};
  socklen_t length = sizeof(credentials);
  if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0)
    return std::unexpected{make_error(errno)};
  return Credentials{credentials.uid, credentials.gid, credentials.pid};
}

inline expected<void> prepare_native_pipe(int descriptor) noexcept {
  return prepare_socket_descriptor(descriptor);
}

inline int create_native_pipe(int (&descriptors)[2]) noexcept {
  return ::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC);
}
}
