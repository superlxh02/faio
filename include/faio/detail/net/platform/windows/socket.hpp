/**
 * @file faio/detail/net/platform/windows/socket.hpp
 * @brief 网络地址、socket 与传输协议：socket 资源封装。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
#include "faio/detail/io/platform/native_types.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/platform/windows/socket.hpp"
#include "faio/detail/io/platform/windows/error.hpp"
#include <iphlpapi.h>
#include <algorithm>
#include <climits>
#include <optional>

namespace faio::net::detail {
/** @brief socket 的完整原生描述符类型，Windows 禁止缩窄为 POSIX int。 */
using native_socket_type = io::detail::native_descriptor;

/** @brief 获取最近 socket syscall 错误；Windows 明确使用 Winsock 错误域。 */
inline auto socket_error() noexcept -> Error {
  return Error{::WSAGetLastError(), 0, error_domain::winsock};
}

/** @brief 平台中立 would-block 判断，同时识别显式用户 syscall 的 errno 错误。 */
inline auto socket_would_block(const Error& error) noexcept -> bool {
  if (error.domain() == error_domain::winsock)
    return error.value() == WSAEWOULDBLOCK;
  return error.value() == EAGAIN || error.value() == EWOULDBLOCK;
}

/** @brief socket 控制选项只转换字节指针，保持两平台原生值和长度语义。 */
inline auto socket_setsockopt(native_socket_type descriptor,
                              int level,
                              int option,
                              const void* value,
                              socklen_t length) noexcept -> int {
  return ::setsockopt(
      io::windows::as_socket(descriptor), level, option, static_cast<const char*>(value), length);
}

inline auto socket_getsockopt(native_socket_type descriptor,
                              int level,
                              int option,
                              void* value,
                              socklen_t* length) noexcept -> int {
  return ::getsockopt(
      io::windows::as_socket(descriptor), level, option, static_cast<char*>(value), length);
}

/** @brief 查询 socket 地址族；Windows 未绑定 socket 不能用 getsockname 推断。
 * @details SO_PROTOCOL_INFOW 在未绑定阶段也可读取，保留 import、TTL 的配置合同。
 */
inline auto socket_address_family(native_socket_type descriptor) noexcept -> expected<int> {
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
}

/** @brief 显式同步 stream syscall；Windows 长范围只提交可表达的前缀，允许短 IO。 */
inline auto socket_recv(native_socket_type descriptor,
                        void* buffer,
                        std::size_t length,
                        int flags) noexcept {
  return ::recv(io::windows::as_socket(descriptor),
                static_cast<char*>(buffer),
                static_cast<int>(std::min(length, std::size_t{INT_MAX})),
                flags & ~MSG_DONTWAIT);
}

inline auto socket_send(native_socket_type descriptor,
                        const void* buffer,
                        std::size_t length,
                        int flags) noexcept {
  const char empty{};
  if (!buffer && !length)
    buffer = &empty;
  return ::send(io::windows::as_socket(descriptor),
                static_cast<const char*>(buffer),
                static_cast<int>(std::min(length, std::size_t{INT_MAX})),
                flags & ~MSG_DONTWAIT);
}

/** @brief 数据报不能拆分；超过 Windows int 长度直接报告消息过长。 */
inline auto socket_sendto(native_socket_type descriptor,
                          const void* buffer,
                          std::size_t length,
                          int flags,
                          const sockaddr* address,
                          socklen_t address_length) noexcept {
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
}

inline auto socket_recvmsg(native_socket_type descriptor, msghdr* message, int flags) noexcept {
  return io::windows::recv_message(descriptor, message, flags);
}

inline auto socket_sendmsg(native_socket_type descriptor,
                           const msghdr* message,
                           int flags) noexcept {
  return io::windows::send_message(descriptor, message, flags);
}

inline auto socket_readv(native_socket_type descriptor,
                         const iovec* vectors,
                         std::size_t count) noexcept {
  /// @brief 此辅助接口只用于 stream try_read_vectored；空范围不能变成零字节就绪探测。
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
}

/** @brief close 始终只执行一次；Windows 与 POSIX 使用各自正确的关闭函数。 */
inline void close_native_socket(native_socket_type descriptor) noexcept {
  (void)::closesocket(io::windows::as_socket(descriptor));
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
#elif defined(_WIN32) && defined(IPV6_PKTINFO)
inline constexpr std::optional<int> packet_info_v6_option = IPV6_PKTINFO;
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

inline constexpr auto native_linger_limit = USHRT_MAX;
using multicast_v4_option_value = DWORD;
inline constexpr int no_signal_flags = 0;

inline Error socket_error_from_code(int value) noexcept {
  return io::windows::make_io_error(value);
}

inline bool socket_interrupted(const Error& error) noexcept {
  return error.value() == EINTR || error.value() == WSAEINTR;
}

inline expected<void> initialize_resolver() noexcept {
  return io::windows::initialize_winsock();
}

inline Error resolver_error(int failure) noexcept {
  return Error{failure, 0, error_domain::resolver};
}

/// @brief 为导入或新建 socket 设置平台必要的非阻塞及继承属性。
inline expected<void> prepare_native_socket(native_socket_type descriptor) noexcept {
  return io::windows::prepare_socket(descriptor);
}

/// @brief 创建平台 socket 并在失败时归还已经取得的句柄。
inline expected<native_socket_type> create_native_socket(int domain,
                                                         int type,
                                                         int protocol) noexcept {
  if (auto initialized = io::windows::initialize_winsock(); !initialized)
    return std::unexpected{initialized.error()};
  const auto descriptor = static_cast<native_socket_type>(
      ::WSASocketW(domain,
                   type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC),
                   protocol,
                   nullptr,
                   0,
                   WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
  if (descriptor < 0)
    return std::unexpected{socket_error()};
  return descriptor;
}

/// @brief 尝试接受一个原生连接，结果保留暂不可用与永久错误的区别。
inline native_socket_type accept_native_socket(native_socket_type descriptor,
                                               sockaddr* address,
                                               socklen_t* length) noexcept {
  return static_cast<native_socket_type>(
      ::accept(io::windows::as_socket(descriptor), address, length));
}

template <class Address>
inline void set_native_address_length(Address&) noexcept {}

}
