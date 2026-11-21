/**
 * @file faio/detail/net/platform/unix/socket.hpp
 * @brief 套接字资源封装。
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
/**  @brief socket 的完整原生描述符类型，Windows 禁止缩窄为 POSIX int。 */
using native_socket_type = io::detail::native_descriptor;

/**  @brief 获取最近 socket syscall 错误；Windows 明确使用 Winsock 错误域。 */
inline auto socket_error() noexcept -> Error {
  return make_error(errno);
}

/**  @brief 不依赖平台的 would-block 判断，同时识别显式用户 syscall 的 errno 错误。 */
inline auto socket_would_block(const Error& error) noexcept -> bool {
  return error.value() == EAGAIN || error.value() == EWOULDBLOCK;
}

/**  @brief socket 控制选项只转换字节指针，不改变两平台的原生值和长度语义。 */
inline auto socket_setsockopt(native_socket_type descriptor,
                              int level,
                              int option,
                              const void* value,
                              socklen_t length) noexcept -> int {
  return ::setsockopt(descriptor, level, option, value, length);
}

/** @brief 调用原生 getsockopt 并保留平台参数约定。 */
inline auto socket_getsockopt(native_socket_type descriptor,
                              int level,
                              int option,
                              void* value,
                              socklen_t* length) noexcept -> int {
  return ::getsockopt(descriptor, level, option, value, length);
}

/**
 *  @brief 查询 socket 地址族；Windows 未绑定 socket 不能用 getsockname 推断。
 * @details SO_PROTOCOL_INFOW 在未绑定阶段也可读取，保留 import、TTL 的配置约定。
 */
inline auto socket_address_family(native_socket_type descriptor) noexcept -> expected<int> {
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) < 0)
    return std::unexpected{socket_error()};
  return address.ss_family;
}

/**  @brief 显式同步 stream syscall；Windows 对较长范围只提交可表示的前缀，允许短 IO。 */
inline auto socket_recv(native_socket_type descriptor,
                        void* buffer,
                        std::size_t length,
                        int flags) noexcept {
  return ::recv(descriptor, buffer, length, flags);
}

/** @brief 发送字节并应用平台的信号和标志处理。 */
inline auto socket_send(native_socket_type descriptor,
                        const void* buffer,
                        std::size_t length,
                        int flags) noexcept {
  return ::send(descriptor, buffer, length, flags);
}

/**  @brief 数据报不能拆分；超过 Windows int 长度直接报告消息过长。 */
inline auto socket_sendto(native_socket_type descriptor,
                          const void* buffer,
                          std::size_t length,
                          int flags,
                          const sockaddr* address,
                          socklen_t address_length) noexcept {
  return ::sendto(descriptor, buffer, length, flags, address, address_length);
}

/** @brief 调用平台消息接收接口。 */
inline auto socket_recvmsg(native_socket_type descriptor, msghdr* message, int flags) noexcept {
  return ::recvmsg(descriptor, message, flags);
}

/** @brief 调用平台消息发送接口。 */
inline auto socket_sendmsg(native_socket_type descriptor,
                           const msghdr* message,
                           int flags) noexcept {
  return ::sendmsg(descriptor, message, flags);
}

/** @brief 向多个缓冲区分散读取字节。 */
inline auto socket_readv(native_socket_type descriptor,
                         const iovec* vectors,
                         std::size_t count) noexcept {
  return ::readv(descriptor, vectors, static_cast<int>(count));
}

/**  @brief close 始终只执行一次；Windows 与 POSIX 使用各自正确的关闭函数。 */
inline void close_native_socket(native_socket_type descriptor) noexcept {
  (void)::close(descriptor);
}

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

/** @brief 把原生 socket 错误码转换为 faio 错误。 */
inline Error socket_error_from_code(int value) noexcept {
  return make_error(value);
}

/** @brief 判断原生 socket 错误是否表示系统调用被中断。 */
inline bool socket_interrupted(const Error& error) noexcept {
  return error.value() == EINTR;
}

/** @brief 初始化平台的名称解析运行环境。 */
inline expected<void> initialize_resolver() noexcept {
  return {};
}

/** @brief 将名称解析错误转换为对应错误域。 */
inline Error resolver_error(int failure) noexcept {
  return failure == EAI_SYSTEM ? socket_error() : Error{failure, 0, error_domain::resolver};
}

/** @brief 为导入的 socket 准备非阻塞及继承标志。 */
inline expected<void> prepare_socket_descriptor(native_socket_type descriptor) noexcept {
  const int status = ::fcntl(descriptor, F_GETFL, 0);
  if (status < 0 || ::fcntl(descriptor, F_SETFL, status | O_NONBLOCK) < 0)
    return std::unexpected{socket_error()};
  const int flags = ::fcntl(descriptor, F_GETFD, 0);
  if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0)
    return std::unexpected{socket_error()};
  return {};
}

}  // namespace faio::net::detail
