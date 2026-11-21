/**
 * @file faio/detail/net/platform/unix/bsd_socket.hpp
 * @brief 套接字资源封装。
 */
#pragma once
#include "faio/detail/net/platform/unix/socket.hpp"
#include <cstddef>
#include <string_view>

namespace faio::net::detail {
/** @brief 为导入或新建 socket 设置平台必要的非阻塞及继承属性。 */
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

/** @brief BSD 创建或 accept 时不保证原子设置标志，仍须完整准备并配置 SIGPIPE。 */
inline expected<void> prepare_created_native_socket(native_socket_type descriptor) noexcept {
  return prepare_native_socket(descriptor);
}

/** @brief 创建平台 socket 并在失败时归还已经取得的句柄。 */
inline expected<native_socket_type> create_native_socket(int domain,
                                                         int type,
                                                         int protocol) noexcept {
  const int descriptor = ::socket(domain, type, protocol);
  if (descriptor < 0)
    return std::unexpected{socket_error()};
  return descriptor;
}

/** @brief 尝试接受一个原生连接，结果保留暂不可用与永久错误的区别。 */
inline native_socket_type accept_native_socket(native_socket_type descriptor,
                                               sockaddr* address,
                                               socklen_t* length) noexcept {
  return ::accept(descriptor, address, length);
}

/** @brief 按平台约定处理原生 socket 地址的长度字段。 */
template <class Address>
inline void set_native_address_length(Address& address) noexcept {
  if constexpr (requires { address.sin_len; })
    address.sin_len = sizeof(address);
  else if constexpr (requires { address.sin6_len; })
    address.sin6_len = sizeof(address);
}

/** @brief 按平台约定处理原生 socket 地址的长度字段。 */
inline void set_native_address_length(sockaddr_un& address, socklen_t length) noexcept {
  address.sun_len = static_cast<unsigned char>(length);
}

/** @brief 构造 Linux 抽象 Unix 地址，不支持的平台返回错误。 */
inline expected<socklen_t> make_abstract_address(sockaddr_un&, std::string_view) {
  return std::unexpected{make_error(ENOTSUP)};
}

/** @brief 查询 Unix 连接对端的进程或用户凭据。 */
template <class Credentials>
inline expected<Credentials> socket_peer_credentials(native_socket_type descriptor) noexcept {
  uid_t uid{};
  gid_t gid{};
  if (::getpeereid(descriptor, &uid, &gid) < 0)
    return std::unexpected{make_error(errno)};
  return Credentials{uid, gid, std::nullopt};
}

/** @brief 将管道描述符设置为非阻塞并在 exec 时关闭。 */
inline expected<void> prepare_native_pipe(int descriptor) noexcept {
  if (auto prepared = prepare_socket_descriptor(descriptor); !prepared)
    return prepared;
#ifdef F_SETNOSIGPIPE
  if (::fcntl(descriptor, F_SETNOSIGPIPE, 1) < 0)
    return std::unexpected{make_error(errno)};
#endif
  return {};
}

/** @brief 创建非阻塞且在 exec 时关闭的原生管道两端。 */
inline int create_native_pipe(int (&descriptors)[2]) noexcept {
  return ::pipe(descriptors);
}
}  // namespace faio::net::detail
