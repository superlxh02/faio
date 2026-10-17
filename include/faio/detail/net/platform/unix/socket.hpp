/**
 * @file faio/detail/net/platform/unix/socket.hpp
 * @brief 网络地址、socket 与传输协议：socket 资源封装。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
#include "faio/detail/io/platform/native_types.hpp"
#include "faio/detail/common/error.hpp"
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/un.h>
#include <algorithm>
#include <climits>
#include <optional>

#ifdef unix
#undef unix
#endif

namespace faio::net::detail {
/** @brief socket 的完整原生描述符类型，Windows 禁止缩窄为 POSIX int。 */
using native_socket_type = io::detail::native_descriptor;

/** @brief 获取最近 socket syscall 错误；Windows 明确使用 Winsock 错误域。 */
inline auto socket_error() noexcept -> Error {
  return make_error(errno);
}

/** @brief 平台中立 would-block 判断，同时识别显式用户 syscall 的 errno 错误。 */
inline auto socket_would_block(const Error& error) noexcept -> bool {
  return error.value() == EAGAIN || error.value() == EWOULDBLOCK;
}

/** @brief socket 控制选项只转换字节指针，保持两平台原生值和长度语义。 */
inline auto socket_setsockopt(native_socket_type descriptor,
                              int level,
                              int option,
                              const void* value,
                              socklen_t length) noexcept -> int {
  return ::setsockopt(descriptor, level, option, value, length);
}

inline auto socket_getsockopt(native_socket_type descriptor,
                              int level,
                              int option,
                              void* value,
                              socklen_t* length) noexcept -> int {
  return ::getsockopt(descriptor, level, option, value, length);
}

/** @brief 查询 socket 地址族；Windows 未绑定 socket 不能用 getsockname 推断。
 * @details SO_PROTOCOL_INFOW 在未绑定阶段也可读取，保留 import、TTL 的配置合同。
 */
inline auto socket_address_family(native_socket_type descriptor) noexcept -> expected<int> {
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) < 0)
    return std::unexpected{socket_error()};
  return address.ss_family;
}

/** @brief 显式同步 stream syscall；Windows 长范围只提交可表达的前缀，允许短 IO。 */
inline auto socket_recv(native_socket_type descriptor,
                        void* buffer,
                        std::size_t length,
                        int flags) noexcept {
  return ::recv(descriptor, buffer, length, flags);
}

inline auto socket_send(native_socket_type descriptor,
                        const void* buffer,
                        std::size_t length,
                        int flags) noexcept {
  return ::send(descriptor, buffer, length, flags);
}

/** @brief 数据报不能拆分；超过 Windows int 长度直接报告消息过长。 */
inline auto socket_sendto(native_socket_type descriptor,
                          const void* buffer,
                          std::size_t length,
                          int flags,
                          const sockaddr* address,
                          socklen_t address_length) noexcept {
  return ::sendto(descriptor, buffer, length, flags, address, address_length);
}

inline auto socket_recvmsg(native_socket_type descriptor, msghdr* message, int flags) noexcept {
  return ::recvmsg(descriptor, message, flags);
}

inline auto socket_sendmsg(native_socket_type descriptor,
                           const msghdr* message,
                           int flags) noexcept {
  return ::sendmsg(descriptor, message, flags);
}

inline auto socket_readv(native_socket_type descriptor,
                         const iovec* vectors,
                         std::size_t count) noexcept {
  return ::readv(descriptor, vectors, static_cast<int>(count));
}

/** @brief close 始终只执行一次；Windows 与 POSIX 使用各自正确的关闭函数。 */
inline void close_native_socket(native_socket_type descriptor) noexcept {
  (void)::close(descriptor);
}

/// @brief 选项的存在性属于原生平台合同；公共 mixin 只观察这些编译期描述。
#ifdef SO_REUSEPORT
inline constexpr std::optional<int> reuseport_option = SO_REUSEPORT;
#else
inline constexpr std::optional<int> reuseport_option = std::nullopt;
#endif
#ifdef IP_PKTINFO
inline constexpr std::optional<int> packet_info_v4_option = IP_PKTINFO;
#elif defined(IP_RECVDSTADDR)
inline constexpr std::optional<int> packet_info_v4_option = IP_RECVDSTADDR;
#else
inline constexpr std::optional<int> packet_info_v4_option = std::nullopt;
#endif
#ifdef IPV6_RECVPKTINFO
inline constexpr std::optional<int> packet_info_v6_option = IPV6_RECVPKTINFO;
#else
inline constexpr std::optional<int> packet_info_v6_option = std::nullopt;
#endif
#ifdef SO_PASSCRED
inline constexpr std::optional<int> passcred_option = SO_PASSCRED;
#else
inline constexpr std::optional<int> passcred_option = std::nullopt;
#endif
#ifdef SO_MARK
inline constexpr std::optional<int> mark_option = SO_MARK;
#else
inline constexpr std::optional<int> mark_option = std::nullopt;
#endif

#ifdef MSG_NOSIGNAL
inline constexpr int no_signal_flags = MSG_NOSIGNAL;
#else
inline constexpr int no_signal_flags = 0;
#endif
inline constexpr auto native_linger_limit = INT_MAX;
using multicast_v4_option_value = unsigned char;

inline Error socket_error_from_code(int value) noexcept {
  return make_error(value);
}

inline bool socket_interrupted(const Error& error) noexcept {
  return error.value() == EINTR;
}

inline expected<void> initialize_resolver() noexcept {
  return {};
}

inline Error resolver_error(int failure) noexcept {
  return failure == EAI_SYSTEM ? socket_error() : Error{failure, 0, error_domain::resolver};
}

inline expected<void> prepare_socket_descriptor(native_socket_type descriptor) noexcept {
  const int status = ::fcntl(descriptor, F_GETFL, 0);
  if (status < 0 || ::fcntl(descriptor, F_SETFL, status | O_NONBLOCK) < 0)
    return std::unexpected{socket_error()};
  const int flags = ::fcntl(descriptor, F_GETFD, 0);
  if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0)
    return std::unexpected{socket_error()};
  return {};
}

}
