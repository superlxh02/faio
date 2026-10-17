/**
 * @file faio/detail/net/platform/macos/socket.hpp
 * @brief 网络地址、socket 与传输协议：socket 资源封装。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
#include "faio/detail/net/platform/unix/socket.hpp"
#include <cstddef>
#include <string_view>

namespace faio::net::detail {
/// @brief 为导入或新建 socket 设置平台必要的非阻塞及继承属性。
inline expected<void> prepare_native_socket(native_socket_type descriptor) noexcept {
  if (auto prepared = prepare_socket_descriptor(descriptor); !prepared)
    return prepared;
#ifdef SO_NOSIGPIPE
  const int enabled = 1;
  if (socket_setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
    return std::unexpected{socket_error()};
#endif
  return {};
}

/// @brief 创建平台 socket 并在失败时归还已经取得的句柄。
inline expected<native_socket_type> create_native_socket(int domain,
                                                         int type,
                                                         int protocol) noexcept {
  const int descriptor = ::socket(domain, type, protocol);
  if (descriptor < 0)
    return std::unexpected{socket_error()};
  return descriptor;
}

/// @brief 尝试接受一个原生连接，结果保留暂不可用与永久错误的区别。
inline native_socket_type accept_native_socket(native_socket_type descriptor,
                                               sockaddr* address,
                                               socklen_t* length) noexcept {
  return ::accept(descriptor, address, length);
}

template <class Address>
inline void set_native_address_length(Address& address) noexcept {
  if constexpr (requires { address.sin_len; })
    address.sin_len = sizeof(address);
  else if constexpr (requires { address.sin6_len; })
    address.sin6_len = sizeof(address);
}

inline void set_native_address_length(sockaddr_un& address, socklen_t length) noexcept {
  address.sun_len = static_cast<unsigned char>(length);
}

inline expected<socklen_t> make_abstract_address(sockaddr_un&, std::string_view) {
  return std::unexpected{make_error(ENOTSUP)};
}

template <class Credentials>
inline expected<Credentials> socket_peer_credentials(native_socket_type descriptor) noexcept {
  uid_t uid{};
  gid_t gid{};
  if (::getpeereid(descriptor, &uid, &gid) < 0)
    return std::unexpected{make_error(errno)};
  return Credentials{uid, gid, std::nullopt};
}

inline expected<void> prepare_native_pipe(int descriptor) noexcept {
  if (auto prepared = prepare_socket_descriptor(descriptor); !prepared)
    return prepared;
#ifdef F_SETNOSIGPIPE
  if (::fcntl(descriptor, F_SETNOSIGPIPE, 1) < 0)
    return std::unexpected{make_error(errno)};
#endif
  return {};
}

inline int create_native_pipe(int (&descriptors)[2]) noexcept {
  return ::pipe(descriptors);
}
}
