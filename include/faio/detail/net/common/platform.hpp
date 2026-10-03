#pragma once
#include "faio/detail/io/platform/posix_types.hpp"
#include "faio/detail/common/error.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_socket.hpp"
#include "faio/detail/io/platform/windows_error.hpp"
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <climits>

namespace faio::net::detail {
/** @brief socket 的完整原生描述符类型，Windows 禁止缩窄为 POSIX int。 */
using native_socket_type = io::detail::native_descriptor;

/** @brief 获取最近 socket syscall 错误；Windows 明确使用 Winsock 错误域。 */
inline auto socket_error() noexcept -> Error {
#if defined(_WIN32)
  return Error{::WSAGetLastError(), 0, error_domain::winsock};
#else
  return make_error(errno);
#endif
}

/** @brief 平台中立 would-block 判断，同时识别显式用户 syscall 的 errno 错误。 */
inline auto socket_would_block(const Error& error) noexcept -> bool {
#if defined(_WIN32)
  if (error.domain() == error_domain::winsock)
    return error.value() == WSAEWOULDBLOCK;
#endif
  return error.value() == EAGAIN || error.value() == EWOULDBLOCK;
}

/** @brief socket 控制选项只转换字节指针，保持两平台原生值和长度语义。 */
inline auto socket_setsockopt(native_socket_type descriptor,
                              int level,
                              int option,
                              const void* value,
                              socklen_t length) noexcept -> int {
#if defined(_WIN32)
  return ::setsockopt(
      io::windows::as_socket(descriptor), level, option, static_cast<const char*>(value), length);
#else
  return ::setsockopt(descriptor, level, option, value, length);
#endif
}

inline auto socket_getsockopt(native_socket_type descriptor,
                              int level,
                              int option,
                              void* value,
                              socklen_t* length) noexcept -> int {
#if defined(_WIN32)
  return ::getsockopt(
      io::windows::as_socket(descriptor), level, option, static_cast<char*>(value), length);
#else
  return ::getsockopt(descriptor, level, option, value, length);
#endif
}

/** @brief 查询 socket 地址族；Windows 未绑定 socket 不能用 getsockname 推断。
 * @details SO_PROTOCOL_INFOW 在未绑定阶段也可读取，保留 import、TTL 的配置合同。
 */
inline auto socket_address_family(native_socket_type descriptor) noexcept -> expected<int> {
#if defined(_WIN32)
  WSAPROTOCOL_INFOW protocol{};
  int length = sizeof(protocol);
  if (::getsockopt(io::windows::as_socket(descriptor),
                   SOL_SOCKET,
                   SO_PROTOCOL_INFOW,
                   reinterpret_cast<char*>(&protocol),
                   &length)
      == SOCKET_ERROR)
    return std::unexpected{socket_error()};
  return protocol.iAddressFamily;
#else
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) < 0)
    return std::unexpected{socket_error()};
  return address.ss_family;
#endif
}

/** @brief 显式同步 stream syscall；Windows 长范围只提交可表达的前缀，允许短 IO。 */
inline auto socket_recv(native_socket_type descriptor,
                        void* buffer,
                        std::size_t length,
                        int flags) noexcept {
#if defined(_WIN32)
  return ::recv(io::windows::as_socket(descriptor),
                static_cast<char*>(buffer),
                static_cast<int>(std::min(length, std::size_t{INT_MAX})),
                flags & ~MSG_DONTWAIT);
#else
  return ::recv(descriptor, buffer, length, flags);
#endif
}

inline auto socket_send(native_socket_type descriptor,
                        const void* buffer,
                        std::size_t length,
                        int flags) noexcept {
#if defined(_WIN32)
  const char empty{};
  if (!buffer && !length)
    buffer = &empty;
  return ::send(io::windows::as_socket(descriptor),
                static_cast<const char*>(buffer),
                static_cast<int>(std::min(length, std::size_t{INT_MAX})),
                flags & ~MSG_DONTWAIT);
#else
  return ::send(descriptor, buffer, length, flags);
#endif
}

/** @brief 数据报不能拆分；超过 Windows int 长度直接报告消息过长。 */
inline auto socket_sendto(native_socket_type descriptor,
                          const void* buffer,
                          std::size_t length,
                          int flags,
                          const sockaddr* address,
                          socklen_t address_length) noexcept {
#if defined(_WIN32)
  if (length > INT_MAX)
    return io::windows::socket_failure(WSAEMSGSIZE);
  const char empty{};
  if (!buffer && !length)
    buffer = &empty;
  return ::sendto(io::windows::as_socket(descriptor),
                  static_cast<const char*>(buffer),
                  static_cast<int>(length),
                  flags & ~MSG_DONTWAIT,
                  address,
                  address_length);
#else
  return ::sendto(descriptor, buffer, length, flags, address, address_length);
#endif
}

inline auto socket_recvmsg(native_socket_type descriptor, msghdr* message, int flags) noexcept {
#if defined(_WIN32)
  return io::windows::recv_message(descriptor, message, flags);
#else
  return ::recvmsg(descriptor, message, flags);
#endif
}

inline auto socket_sendmsg(native_socket_type descriptor,
                           const msghdr* message,
                           int flags) noexcept {
#if defined(_WIN32)
  return io::windows::send_message(descriptor, message, flags);
#else
  return ::sendmsg(descriptor, message, flags);
#endif
}

inline auto socket_readv(native_socket_type descriptor,
                         const iovec* vectors,
                         std::size_t count) noexcept {
#if defined(_WIN32)
  // 此辅助接口只用于 stream try_read_vectored；空范围不能变成零字节就绪探测。
  if (!count)
    return std::intptr_t{0};
  if (vectors && count <= IOV_MAX && std::all_of(vectors, vectors + count, [](const iovec& vector) {
        return vector.iov_len == 0;
      }))
    return std::intptr_t{0};
  msghdr message{};
  message.msg_iov = const_cast<iovec*>(vectors);
  message.msg_iovlen = count;
  return io::windows::recv_message(descriptor, &message, 0);
#else
  return ::readv(descriptor, vectors, static_cast<int>(count));
#endif
}

/** @brief close 始终只执行一次；Windows 与 POSIX 使用各自正确的关闭函数。 */
inline void close_native_socket(native_socket_type descriptor) noexcept {
#if defined(_WIN32)
  (void)::closesocket(io::windows::as_socket(descriptor));
#else
  (void)::close(descriptor);
#endif
}
}  // namespace faio::net::detail
