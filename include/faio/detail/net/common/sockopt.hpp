#ifndef FAIO_DETAIL_NET_COMMON_SOCKOPT_HPP
#define FAIO_DETAIL_NET_COMMON_SOCKOPT_HPP
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/io.hpp"
#include "faio/detail/net/common/address.hpp"
#include <chrono>
#include <climits>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <sys/socket.h>

namespace faio::net::detail {
/** @brief 设置短控制选项；errno 必须在下一次 syscall 前保存。 */
[[nodiscard]] inline auto set_sock_opt(int fd, int level, int option,
                                       const void *value,
                                       socklen_t length) noexcept
    -> expected<void> {
  if (::setsockopt(fd, level, option, value, length) < 0)
    return std::unexpected{make_error(errno)};
  return {};
}
[[nodiscard]] inline auto get_sock_opt(int fd, int level, int option,
                                       void *value, socklen_t length) noexcept
    -> expected<void> {
  if (::getsockopt(fd, level, option, value, &length) < 0)
    return std::unexpected{make_error(errno)};
  return {};
}
/** @brief 网络控制选项 CRTP；不为短 syscall 创建线程池任务。 */
template <class T> struct ImplSocketOptions {
private:
  [[nodiscard]] auto descriptor() const noexcept -> int {
    return static_cast<const T *>(this)->fd();
  }
  /** @brief 控制 syscall 与资源关闭在同一 domain 临界区串行，避免 fd 重用竞态。
   */
  auto set_option(int level, int option, const void *value,
                  socklen_t length) const noexcept -> expected<void> {
    return io::detail::with_resource(
        static_cast<const T *>(this)->resource(), io::Interest::none, [&] {
          return set_sock_opt(descriptor(), level, option, value, length);
        });
  }
  auto get_option(int level, int option, void *value,
                  socklen_t length) const noexcept -> expected<void> {
    return io::detail::with_resource(
        static_cast<const T *>(this)->resource(), io::Interest::none, [&] {
          return get_sock_opt(descriptor(), level, option, value, length);
        });
  }
  auto family() const noexcept -> expected<int> {
    return io::detail::with_resource(
        static_cast<const T *>(this)->resource(), io::Interest::none,
        [&]() -> expected<int> {
          sockaddr_storage address{};
          socklen_t length = sizeof(address);
          if (::getsockname(descriptor(),
                            reinterpret_cast<sockaddr *>(&address),
                            &length) < 0)
            return std::unexpected{make_error(errno)};
          return address.ss_family;
        });
  }
  [[nodiscard]] auto integer(int level, int option) const noexcept
      -> expected<int> {
    int value{};
    if (auto result = get_option(level, option, &value, sizeof(value)); !result)
      return std::unexpected{result.error()};
    return value;
  }
  [[nodiscard]] auto boolean(int level, int option) const noexcept
      -> expected<bool> {
    auto result = integer(level, option);
    if (!result)
      return std::unexpected{result.error()};
    return *result != 0;
  }
  auto set_integer(int level, int option, int value) noexcept
      -> expected<void> {
    return set_option(level, option, &value, sizeof(value));
  }

public:
  /** @brief 禁用 Nagle；小包 request/response 通常需要开启。 */
  auto set_nodelay(bool on) noexcept -> expected<void> {
    return set_integer(IPPROTO_TCP, TCP_NODELAY, on);
  }
  [[nodiscard]] auto nodelay() const noexcept -> expected<bool> {
    return boolean(IPPROTO_TCP, TCP_NODELAY);
  }
  auto set_keepalive(bool on) noexcept -> expected<void> {
    return set_integer(SOL_SOCKET, SO_KEEPALIVE, on);
  }
  [[nodiscard]] auto keepalive() const noexcept -> expected<bool> {
    return boolean(SOL_SOCKET, SO_KEEPALIVE);
  }
  auto set_recv_buffer_size(int size) noexcept -> expected<void> {
    if (size <= 0)
      return std::unexpected{make_error(EINVAL)};
    return set_integer(SOL_SOCKET, SO_RCVBUF, size);
  }
  auto set_send_buffer_size(int size) noexcept -> expected<void> {
    if (size <= 0)
      return std::unexpected{make_error(EINVAL)};
    return set_integer(SOL_SOCKET, SO_SNDBUF, size);
  }
  [[nodiscard]] auto recv_buffer_size() const noexcept
      -> expected<std::size_t> {
    auto value = integer(SOL_SOCKET, SO_RCVBUF);
    if (!value)
      return std::unexpected{value.error()};
    return static_cast<std::size_t>(*value);
  }
  [[nodiscard]] auto send_buffer_size() const noexcept
      -> expected<std::size_t> {
    auto value = integer(SOL_SOCKET, SO_SNDBUF);
    if (!value)
      return std::unexpected{value.error()};
    return static_cast<std::size_t>(*value);
  }
  /** @brief nullopt 关闭 linger；0 秒表示 abortive close；正数保持 OS
   * 原生语义。 */
  auto set_linger(std::optional<std::chrono::seconds> duration) noexcept
      -> expected<void> {
    struct ::linger value{};
    if (duration) {
      if (duration->count() < 0 || duration->count() > INT_MAX)
        return std::unexpected{make_error(EINVAL)};
      value.l_onoff = 1;
      value.l_linger = static_cast<int>(duration->count());
    }
    return set_option(SOL_SOCKET, SO_LINGER, &value, sizeof(value));
  }
  [[nodiscard]] auto linger() const noexcept
      -> expected<std::optional<std::chrono::seconds>> {
    struct ::linger value{};
    if (auto result = get_option(SOL_SOCKET, SO_LINGER, &value, sizeof(value));
        !result)
      return std::unexpected{result.error()};
    if (!value.l_onoff)
      return std::optional<std::chrono::seconds>{};
    return std::optional{std::chrono::seconds{value.l_linger}};
  }
  auto set_reuseaddr(bool on) noexcept -> expected<void> {
    return set_integer(SOL_SOCKET, SO_REUSEADDR, on);
  }
  [[nodiscard]] auto reuseaddr() const noexcept -> expected<bool> {
    return boolean(SOL_SOCKET, SO_REUSEADDR);
  }
  auto set_reuseport(bool on) noexcept -> expected<void> {
#ifdef SO_REUSEPORT
    return set_integer(SOL_SOCKET, SO_REUSEPORT, on);
#else
    (void)on;
    return std::unexpected{make_error(ENOTSUP)};
#endif
  }
  [[nodiscard]] auto reuseport() const noexcept -> expected<bool> {
#ifdef SO_REUSEPORT
    return boolean(SOL_SOCKET, SO_REUSEPORT);
#else
    return std::unexpected{make_error(ENOTSUP)};
#endif
  }
  auto set_broadcast(bool on) noexcept -> expected<void> {
    return set_integer(SOL_SOCKET, SO_BROADCAST, on);
  }
  [[nodiscard]] auto broadcast() const noexcept -> expected<bool> {
    return boolean(SOL_SOCKET, SO_BROADCAST);
  }
  auto set_only_v6(bool on) noexcept -> expected<void> {
    return set_integer(IPPROTO_IPV6, IPV6_V6ONLY, on);
  }
  [[nodiscard]] auto only_v6() const noexcept -> expected<bool> {
    return boolean(IPPROTO_IPV6, IPV6_V6ONLY);
  }
  /** @brief IP TTL/hop limit 自动依据 socket 地址族选择。 */
  auto set_ttl(std::uint32_t value) noexcept -> expected<void> {
    if (value == 0 || value > 255)
      return std::unexpected{make_error(EINVAL)};
    const auto socket_family = family();
    if (!socket_family)
      return std::unexpected{socket_family.error()};
    return *socket_family == AF_INET6
               ? set_integer(IPPROTO_IPV6, IPV6_UNICAST_HOPS,
                             static_cast<int>(value))
               : set_integer(IPPROTO_IP, IP_TTL, static_cast<int>(value));
  }
  [[nodiscard]] auto ttl() const noexcept -> expected<std::uint32_t> {
    const auto socket_family = family();
    if (!socket_family)
      return std::unexpected{socket_family.error()};
    auto value = *socket_family == AF_INET6
                     ? integer(IPPROTO_IPV6, IPV6_UNICAST_HOPS)
                     : integer(IPPROTO_IP, IP_TTL);
    if (!value)
      return std::unexpected{value.error()};
    return static_cast<std::uint32_t>(*value);
  }
  /** @brief 读取并清除挂起的 SO_ERROR；没有错误时返回 nullopt。 */
  [[nodiscard]] auto take_error() const noexcept
      -> expected<std::optional<Error>> {
    auto value = integer(SOL_SOCKET, SO_ERROR);
    if (!value)
      return std::unexpected{value.error()};
    if (*value == 0)
      return std::optional<Error>{};
    return std::optional{make_error(*value)};
  }
  auto join_multicast_v4(Ipv4Addr group,
                         Ipv4Addr interface = Ipv4Addr{}) noexcept
      -> expected<void> {
    ip_mreq value{};
    value.imr_multiaddr.s_addr = group.addr();
    value.imr_interface.s_addr = interface.addr();
    return set_option(IPPROTO_IP, IP_ADD_MEMBERSHIP, &value, sizeof(value));
  }
  auto leave_multicast_v4(Ipv4Addr group,
                          Ipv4Addr interface = Ipv4Addr{}) noexcept
      -> expected<void> {
    ip_mreq value{};
    value.imr_multiaddr.s_addr = group.addr();
    value.imr_interface.s_addr = interface.addr();
    return set_option(IPPROTO_IP, IP_DROP_MEMBERSHIP, &value, sizeof(value));
  }
  auto join_multicast_v6(const Ipv6Addr &group,
                         std::uint32_t interface = 0) noexcept
      -> expected<void> {
    ipv6_mreq value{};
    value.ipv6mr_multiaddr = group.addr();
    value.ipv6mr_interface = interface;
    return set_option(IPPROTO_IPV6, IPV6_JOIN_GROUP, &value, sizeof(value));
  }
  auto leave_multicast_v6(const Ipv6Addr &group,
                          std::uint32_t interface = 0) noexcept
      -> expected<void> {
    ipv6_mreq value{};
    value.ipv6mr_multiaddr = group.addr();
    value.ipv6mr_interface = interface;
    return set_option(IPPROTO_IPV6, IPV6_LEAVE_GROUP, &value, sizeof(value));
  }
  auto set_multicast_loop_v4(bool on) noexcept -> expected<void> {
    const unsigned char value = on;
    return set_option(IPPROTO_IP, IP_MULTICAST_LOOP, &value, sizeof(value));
  }
  [[nodiscard]] auto multicast_loop_v4() const noexcept -> expected<bool> {
    unsigned char value{};
    if (auto result =
            get_option(IPPROTO_IP, IP_MULTICAST_LOOP, &value, sizeof(value));
        !result)
      return std::unexpected{result.error()};
    return value != 0;
  }
  auto set_multicast_ttl_v4(std::uint32_t ttl) noexcept -> expected<void> {
    if (ttl > 255)
      return std::unexpected{make_error(EINVAL)};
    const unsigned char value = static_cast<unsigned char>(ttl);
    return set_option(IPPROTO_IP, IP_MULTICAST_TTL, &value, sizeof(value));
  }
  [[nodiscard]] auto multicast_ttl_v4() const noexcept
      -> expected<std::uint32_t> {
    unsigned char value{};
    if (auto result =
            get_option(IPPROTO_IP, IP_MULTICAST_TTL, &value, sizeof(value));
        !result)
      return std::unexpected{result.error()};
    return static_cast<std::uint32_t>(value);
  }
  auto set_multicast_loop_v6(bool on) noexcept -> expected<void> {
    return set_integer(IPPROTO_IPV6, IPV6_MULTICAST_LOOP, on);
  }
  [[nodiscard]] auto multicast_loop_v6() const noexcept -> expected<bool> {
    return boolean(IPPROTO_IPV6, IPV6_MULTICAST_LOOP);
  }
  auto set_multicast_hops_v6(std::uint32_t value) noexcept -> expected<void> {
    if (value > 255)
      return std::unexpected{make_error(EINVAL)};
    return set_integer(IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                       static_cast<int>(value));
  }
  [[nodiscard]] auto multicast_hops_v6() const noexcept
      -> expected<std::uint32_t> {
    auto value = integer(IPPROTO_IPV6, IPV6_MULTICAST_HOPS);
    if (!value)
      return std::unexpected{value.error()};
    return static_cast<std::uint32_t>(*value);
  }
  auto set_multicast_interface_v4(Ipv4Addr interface) noexcept
      -> expected<void> {
    in_addr value{interface.addr()};
    return set_option(IPPROTO_IP, IP_MULTICAST_IF, &value, sizeof(value));
  }
  auto set_multicast_interface_v6(std::uint32_t interface) noexcept
      -> expected<void> {
    return set_option(IPPROTO_IPV6, IPV6_MULTICAST_IF, &interface,
                      sizeof(interface));
  }
  /** @brief 启用 recv_message 的目标地址/接口辅助数据，保留平台原生 cmsghdr
   * 格式。 */
  auto set_recv_packet_info_v4(bool on) noexcept -> expected<void> {
#if defined(IP_PKTINFO)
    return set_integer(IPPROTO_IP, IP_PKTINFO, on);
#elif defined(IP_RECVDSTADDR)
    return set_integer(IPPROTO_IP, IP_RECVDSTADDR, on);
#else
    (void)on;
    return std::unexpected{make_error(ENOTSUP)};
#endif
  }
  auto set_recv_packet_info_v6(bool on) noexcept -> expected<void> {
#ifdef IPV6_RECVPKTINFO
    return set_integer(IPPROTO_IPV6, IPV6_RECVPKTINFO, on);
#else
    (void)on;
    return std::unexpected{make_error(ENOTSUP)};
#endif
  }
  auto set_passcred(bool on) noexcept -> expected<void> {
#ifdef SO_PASSCRED
    return set_integer(SOL_SOCKET, SO_PASSCRED, on);
#else
    (void)on;
    return std::unexpected{make_error(ENOTSUP)};
#endif
  }
  [[nodiscard]] auto passcred() const noexcept -> expected<bool> {
#ifdef SO_PASSCRED
    return boolean(SOL_SOCKET, SO_PASSCRED);
#else
    return std::unexpected{make_error(ENOTSUP)};
#endif
  }
  auto set_mark(std::uint32_t mark) noexcept -> expected<void> {
#ifdef SO_MARK
    return set_option(SOL_SOCKET, SO_MARK, &mark, sizeof(mark));
#else
    (void)mark;
    return std::unexpected{make_error(ENOTSUP)};
#endif
  }
};
// 旧 CRTP 名称仅为源码兼容保留；新对象统一继承一个 option mixin。
template <class T> using ImplNodelay = ImplSocketOptions<T>;
template <class T> using ImplPasscred = ImplSocketOptions<T>;
template <class T> using ImplRecvBufSize = ImplSocketOptions<T>;
template <class T> using ImplSendBufSize = ImplSocketOptions<T>;
template <class T> using ImplKeepalive = ImplSocketOptions<T>;
template <class T> using ImplLinger = ImplSocketOptions<T>;
template <class T> using ImplBoradcast = ImplSocketOptions<T>;
template <class T> using ImplTTL = ImplSocketOptions<T>;
template <class T> using ImplReuseAddr = ImplSocketOptions<T>;
template <class T> using ImplReusePort = ImplSocketOptions<T>;
template <class T> using ImplMark = ImplSocketOptions<T>;
} // namespace faio::net::detail
#endif
