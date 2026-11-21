/**
 * @file faio/detail/net/platform/linux/socket.hpp
 * @brief 套接字资源封装。
 */
#pragma once
#include "faio/detail/net/platform/unix/socket.hpp"
#include <cstddef>
#include <cstring>
#include <string_view>

namespace faio::net::detail {
/** @brief 为导入或新建 socket 设置平台必要的非阻塞及继承属性。 */
inline expected<void> prepare_native_socket(native_socket_type descriptor) noexcept {
  return prepare_socket_descriptor(descriptor);
}

/**
 *  @brief 内部 socket/accept4/SOCKET SQE 已原子设置这两个标志，无需重复执行 fcntl。
 * @details 仅用于本库创建的描述符；用户导入仍必须调用 prepare_native_socket。
 */
inline expected<void> prepare_created_native_socket(native_socket_type) noexcept {
  return {};
}

/** @brief 创建平台 socket 并在失败时归还已经取得的句柄。 */
inline expected<native_socket_type> create_native_socket(int domain,
                                                         int type,
                                                         int protocol) noexcept {
  const int descriptor = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
  if (descriptor < 0)
    return std::unexpected{socket_error()};
  return descriptor;
}

/** @brief 尝试接受一个原生连接，结果保留暂不可用与永久错误的区别。 */
inline native_socket_type accept_native_socket(native_socket_type descriptor,
                                               sockaddr* address,
                                               socklen_t* length) noexcept {
  return ::accept4(descriptor, address, length, SOCK_NONBLOCK | SOCK_CLOEXEC);
}

/** @brief 按平台约定处理原生 socket 地址的长度字段。 */
template <class Address>
inline void set_native_address_length(Address&) noexcept {}

/** @brief 按平台约定处理原生 socket 地址的长度字段。 */
inline void set_native_address_length(sockaddr_un&, socklen_t) noexcept {}

/** @brief 构造 Linux 抽象 Unix 地址，不支持的平台返回错误。 */
inline expected<socklen_t> make_abstract_address(sockaddr_un& address, std::string_view name) {
  if (name.size() > sizeof(address.sun_path) - 1)
    return std::unexpected{make_error(ENAMETOOLONG)};
  std::memcpy(address.sun_path + 1, name.data(), name.size());
  return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
}

/** @brief 查询 Unix 连接对端的进程或用户凭据。 */
template <class Credentials>
inline expected<Credentials> socket_peer_credentials(native_socket_type descriptor) noexcept {
  struct ucred credentials{};
  socklen_t length = sizeof(credentials);
  if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0)
    return std::unexpected{make_error(errno)};
  return Credentials{credentials.uid, credentials.gid, credentials.pid};
}

/** @brief 将管道描述符设置为非阻塞并在 exec 时关闭。 */
inline expected<void> prepare_native_pipe(int descriptor) noexcept {
  return prepare_socket_descriptor(descriptor);
}

/** @brief 创建非阻塞且在 exec 时关闭的原生管道两端。 */
inline int create_native_pipe(int (&descriptors)[2]) noexcept {
  return ::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC);
}
}  // namespace faio::net::detail
