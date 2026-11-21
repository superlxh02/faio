/**
 * @file faio/detail/net/socket_options.hpp
 * @brief socket 参数读写。
 */
#ifndef FAIO_DETAIL_NET_SOCKET_OPTIONS_HPP
#define FAIO_DETAIL_NET_SOCKET_OPTIONS_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/core/resource_impl.hpp"
#include "faio/detail/net/base/address_value.hpp"
#include <chrono>
#include <optional>

namespace faio::net::detail {
/**  @brief 设置短控制选项；errno 必须在下一次 syscall 前保存。 */
[[nodiscard]] inline auto set_sock_opt(native_socket_type fd,
                                       int level,
                                       int option,
                                       const void* value,
                                       socklen_t length) noexcept -> expected<void> {
  if (socket_setsockopt(fd, level, option, value, length) < 0)
    return std::unexpected{socket_error()};
  return {};
}

/** @brief 读取原生 socket 选项，将系统失败转换为错误结果。 */
[[nodiscard]] inline auto get_sock_opt(native_socket_type fd,
                                       int level,
                                       int option,
                                       void* value,
                                       socklen_t length) noexcept -> expected<void> {
  if (socket_getsockopt(fd, level, option, value, &length) < 0)
    return std::unexpected{socket_error()};
  return {};
}

/**  @brief 网络控制选项 CRTP；不为短 syscall 创建线程池任务。 */
template <class T>
struct ImplSocketOptions {
 private:
  /** @brief 取得当前对象的原生 socket 句柄，不转移所有权。 */
  [[nodiscard]] auto descriptor() const noexcept -> native_socket_type {
    return static_cast<const T*>(this)->fd();
  }

  /**  @brief 控制 syscall 与资源关闭在同一 domain 临界区串行，避免 fd 重用竞态。 */
  auto set_option(int level, int option, const void* value, socklen_t length) const noexcept
      -> expected<void> {
    return io::detail::with_resource(
        static_cast<const T*>(this)->resource(), io::Interest::none, [&] {
          return set_sock_opt(descriptor(), level, option, value, length);
        });
  }

  /** @brief 读取 socket 选项到调用方提供的缓冲区。 */
  auto get_option(int level, int option, void* value, socklen_t length) const noexcept
      -> expected<void> {
    return io::detail::with_resource(
        static_cast<const T*>(this)->resource(), io::Interest::none, [&] {
          return get_sock_opt(descriptor(), level, option, value, length);
        });
  }

  /** @brief 返回地址或 socket 所属的原生地址族。 */
  auto family() const noexcept -> expected<int> {
    return io::detail::with_resource(
        static_cast<const T*>(this)->resource(), io::Interest::none, [&]() -> expected<int> {
          return socket_address_family(descriptor());
        });
  }

  /** @brief 读取整数形式的 socket 选项。 */
  [[nodiscard]] auto integer(int level, int option) const noexcept -> expected<int> {
    int value{};
    if (auto result = get_option(level, option, &value, sizeof(value)); !result)
      return std::unexpected{result.error()};
    return value;
  }

  /** @brief 读取 socket 选项并转换为布尔值。 */
  [[nodiscard]] auto boolean(int level, int option) const noexcept -> expected<bool> {
    auto result = integer(level, option);
    if (!result)
      return std::unexpected{result.error()};
    return *result != 0;
  }

  /** @brief 设置整数形式的 socket 选项。 */
  auto set_integer(int level, int option, int value) noexcept -> expected<void> {
    return set_option(level, option, &value, sizeof(value));
  }

  /** @brief 设置平台可用的整数选项，缺少支持时返回错误。 */
  auto set_optional_integer(int level, std::optional<int> option, int value) noexcept
      -> expected<void> {
    if (!option)
      return std::unexpected{make_error(ENOTSUP)};
    return set_integer(level, *option, value);
  }

  /** @brief 读取平台可用的布尔选项，缺少支持时返回错误。 */
  auto optional_boolean(int level, std::optional<int> option) const noexcept -> expected<bool> {
    if (!option)
      return std::unexpected{make_error(ENOTSUP)};
    return boolean(level, *option);
  }

 public:
  /**  @brief 设置是否禁用 Nagle；小包请求/响应通常需要开启此选项。 */
  auto set_nodelay(bool on) noexcept -> expected<void> {
    return set_integer(IPPROTO_TCP, TCP_NODELAY, on);
  }

  /** @brief 查询 TCP 是否禁用 Nagle 算法。 */
  [[nodiscard]] auto nodelay() const noexcept -> expected<bool> {
    return boolean(IPPROTO_TCP, TCP_NODELAY);
  }

  /** @brief 启用或关闭 socket 的连接保活探测。 */
  auto set_keepalive(bool on) noexcept -> expected<void> {
    return set_integer(SOL_SOCKET, SO_KEEPALIVE, on);
  }

  /** @brief 查询 socket 是否启用连接保活探测。 */
  [[nodiscard]] auto keepalive() const noexcept -> expected<bool> {
    return boolean(SOL_SOCKET, SO_KEEPALIVE);
  }

  /** @brief 设置接收缓冲区的字节容量。 */
  auto set_recv_buffer_size(int size) noexcept -> expected<void> {
    if (size <= 0)
      return std::unexpected{make_error(EINVAL)};
    return set_integer(SOL_SOCKET, SO_RCVBUF, size);
  }

  /** @brief 设置发送缓冲区的字节容量。 */
  auto set_send_buffer_size(int size) noexcept -> expected<void> {
    if (size <= 0)
      return std::unexpected{make_error(EINVAL)};
    return set_integer(SOL_SOCKET, SO_SNDBUF, size);
  }

  /** @brief 查询系统报告的接收缓冲区字节容量。 */
  [[nodiscard]] auto recv_buffer_size() const noexcept -> expected<std::size_t> {
    auto value = integer(SOL_SOCKET, SO_RCVBUF);
    if (!value)
      return std::unexpected{value.error()};
    return static_cast<std::size_t>(*value);
  }

  /** @brief 查询系统报告的发送缓冲区字节容量。 */
  [[nodiscard]] auto send_buffer_size() const noexcept -> expected<std::size_t> {
    auto value = integer(SOL_SOCKET, SO_SNDBUF);
    if (!value)
      return std::unexpected{value.error()};
    return static_cast<std::size_t>(*value);
  }

  /**
   *  @brief nullopt 关闭 linger；0 秒表示 abortive close；正数保持 OS
   * 原生语义。
   */
  auto set_linger(std::optional<std::chrono::seconds> duration) noexcept -> expected<void> {
    struct ::linger value{};
    if (duration) {
      if (duration->count() < 0 || duration->count() > native_linger_limit)
        return std::unexpected{make_error(EINVAL)};
      value.l_onoff = 1;
      value.l_linger = static_cast<decltype(value.l_linger)>(duration->count());
    }
    return set_option(SOL_SOCKET, SO_LINGER, &value, sizeof(value));
  }

  /** @brief 查询关闭时的延迟发送设置，空值表示禁用。 */
  [[nodiscard]] auto linger() const noexcept -> expected<std::optional<std::chrono::seconds>> {
    struct ::linger value{};
    if (auto result = get_option(SOL_SOCKET, SO_LINGER, &value, sizeof(value)); !result)
      return std::unexpected{result.error()};
    if (!value.l_onoff)
      return std::optional<std::chrono::seconds>{};
    return std::optional{std::chrono::seconds{value.l_linger}};
  }

  /** @brief 设置本地地址复用选项。 */
  auto set_reuseaddr(bool on) noexcept -> expected<void> {
    return set_integer(SOL_SOCKET, SO_REUSEADDR, on);
  }

  /** @brief 查询本地地址复用选项。 */
  [[nodiscard]] auto reuseaddr() const noexcept -> expected<bool> {
    return boolean(SOL_SOCKET, SO_REUSEADDR);
  }

  /** @brief 设置端口复用选项。 */
  auto set_reuseport(bool on) noexcept -> expected<void> {
    return set_optional_integer(SOL_SOCKET, reuseport_option, on);
  }

  /** @brief 查询端口复用选项。 */
  [[nodiscard]] auto reuseport() const noexcept -> expected<bool> {
    return optional_boolean(SOL_SOCKET, reuseport_option);
  }

  /** @brief 设置数据报广播发送权限。 */
  auto set_broadcast(bool on) noexcept -> expected<void> {
    return set_integer(SOL_SOCKET, SO_BROADCAST, on);
  }

  /** @brief 查询数据报广播发送权限。 */
  [[nodiscard]] auto broadcast() const noexcept -> expected<bool> {
    return boolean(SOL_SOCKET, SO_BROADCAST);
  }

  /** @brief 设置 IPv6 socket 是否限制为 IPv6 通信。 */
  auto set_only_v6(bool on) noexcept -> expected<void> {
    return set_integer(IPPROTO_IPV6, IPV6_V6ONLY, on);
  }

  /** @brief 查询 IPv6 socket 是否限制为 IPv6 通信。 */
  [[nodiscard]] auto only_v6() const noexcept -> expected<bool> {
    return boolean(IPPROTO_IPV6, IPV6_V6ONLY);
  }

  /**  @brief 按 socket 地址族自动选择 IP TTL/hop limit。 */
  auto set_ttl(std::uint32_t value) noexcept -> expected<void> {
    if (value == 0 || value > 255)
      return std::unexpected{make_error(EINVAL)};
    const auto socket_family = family();
    if (!socket_family)
      return std::unexpected{socket_family.error()};
    return *socket_family == AF_INET6
               ? set_integer(IPPROTO_IPV6, IPV6_UNICAST_HOPS, static_cast<int>(value))
               : set_integer(IPPROTO_IP, IP_TTL, static_cast<int>(value));
  }

  /** @brief 查询单播数据包的 TTL 或 IPv6 跳数上限。 */
  [[nodiscard]] auto ttl() const noexcept -> expected<std::uint32_t> {
    const auto socket_family = family();
    if (!socket_family)
      return std::unexpected{socket_family.error()};
    auto value = *socket_family == AF_INET6 ? integer(IPPROTO_IPV6, IPV6_UNICAST_HOPS)
                                            : integer(IPPROTO_IP, IP_TTL);
    if (!value)
      return std::unexpected{value.error()};
    return static_cast<std::uint32_t>(*value);
  }

  /**  @brief 读取并清除挂起的 SO_ERROR；没有错误时返回 nullopt。 */
  [[nodiscard]] auto take_error() const noexcept -> expected<std::optional<Error>> {
    auto value = integer(SOL_SOCKET, SO_ERROR);
    if (!value)
      return std::unexpected{value.error()};
    if (*value == 0)
      return std::optional<Error>{};
    return std::optional{make_error(*value)};
  }

  /** @brief 在指定 IPv4 接口加入组播组。 */
  auto join_multicast_v4(Ipv4Addr group, Ipv4Addr network_interface = Ipv4Addr{}) noexcept
      -> expected<void> {
    ip_mreq value{};
    value.imr_multiaddr.s_addr = group.addr();
    value.imr_interface.s_addr = network_interface.addr();
    return set_option(IPPROTO_IP, IP_ADD_MEMBERSHIP, &value, sizeof(value));
  }

  /** @brief 在指定 IPv4 接口退出组播组。 */
  auto leave_multicast_v4(Ipv4Addr group, Ipv4Addr network_interface = Ipv4Addr{}) noexcept
      -> expected<void> {
    ip_mreq value{};
    value.imr_multiaddr.s_addr = group.addr();
    value.imr_interface.s_addr = network_interface.addr();
    return set_option(IPPROTO_IP, IP_DROP_MEMBERSHIP, &value, sizeof(value));
  }

  /** @brief 在指定接口索引加入 IPv6 组播组。 */
  auto join_multicast_v6(const Ipv6Addr& group, std::uint32_t network_interface = 0) noexcept
      -> expected<void> {
    ipv6_mreq value{};
    value.ipv6mr_multiaddr = group.addr();
    value.ipv6mr_interface = network_interface;
    return set_option(IPPROTO_IPV6, IPV6_JOIN_GROUP, &value, sizeof(value));
  }

  /** @brief 在指定接口索引退出 IPv6 组播组。 */
  auto leave_multicast_v6(const Ipv6Addr& group, std::uint32_t network_interface = 0) noexcept
      -> expected<void> {
    ipv6_mreq value{};
    value.ipv6mr_multiaddr = group.addr();
    value.ipv6mr_interface = network_interface;
    return set_option(IPPROTO_IPV6, IPV6_LEAVE_GROUP, &value, sizeof(value));
  }

  /** @brief 设置 IPv4 组播发送是否回送到本机。 */
  auto set_multicast_loop_v4(bool on) noexcept -> expected<void> {
    const multicast_v4_option_value value = on;
    return set_option(IPPROTO_IP, IP_MULTICAST_LOOP, &value, sizeof(value));
  }

  /** @brief 查询 IPv4 组播回送设置。 */
  [[nodiscard]] auto multicast_loop_v4() const noexcept -> expected<bool> {
    multicast_v4_option_value value{};
    if (auto result = get_option(IPPROTO_IP, IP_MULTICAST_LOOP, &value, sizeof(value)); !result)
      return std::unexpected{result.error()};
    return value != 0;
  }

  /** @brief 设置 IPv4 组播数据包的 TTL。 */
  auto set_multicast_ttl_v4(std::uint32_t ttl) noexcept -> expected<void> {
    if (ttl > 255)
      return std::unexpected{make_error(EINVAL)};
    const auto value = static_cast<multicast_v4_option_value>(ttl);
    return set_option(IPPROTO_IP, IP_MULTICAST_TTL, &value, sizeof(value));
  }

  /** @brief 查询 IPv4 组播数据包的 TTL。 */
  [[nodiscard]] auto multicast_ttl_v4() const noexcept -> expected<std::uint32_t> {
    multicast_v4_option_value value{};
    if (auto result = get_option(IPPROTO_IP, IP_MULTICAST_TTL, &value, sizeof(value)); !result)
      return std::unexpected{result.error()};
    return static_cast<std::uint32_t>(value);
  }

  /** @brief 设置 IPv6 组播发送是否回送到本机。 */
  auto set_multicast_loop_v6(bool on) noexcept -> expected<void> {
    return set_integer(IPPROTO_IPV6, IPV6_MULTICAST_LOOP, on);
  }

  /** @brief 查询 IPv6 组播回送设置。 */
  [[nodiscard]] auto multicast_loop_v6() const noexcept -> expected<bool> {
    return boolean(IPPROTO_IPV6, IPV6_MULTICAST_LOOP);
  }

  /** @brief 设置 IPv6 组播数据包的跳数上限。 */
  auto set_multicast_hops_v6(std::uint32_t value) noexcept -> expected<void> {
    if (value > 255)
      return std::unexpected{make_error(EINVAL)};
    return set_integer(IPPROTO_IPV6, IPV6_MULTICAST_HOPS, static_cast<int>(value));
  }

  /** @brief 查询 IPv6 组播数据包的跳数上限。 */
  [[nodiscard]] auto multicast_hops_v6() const noexcept -> expected<std::uint32_t> {
    auto value = integer(IPPROTO_IPV6, IPV6_MULTICAST_HOPS);
    if (!value)
      return std::unexpected{value.error()};
    return static_cast<std::uint32_t>(*value);
  }

  /** @brief 指定发送 IPv4 组播使用的本地接口地址。 */
  auto set_multicast_interface_v4(Ipv4Addr network_interface) noexcept -> expected<void> {
    in_addr value{};
    value.s_addr = network_interface.addr();
    return set_option(IPPROTO_IP, IP_MULTICAST_IF, &value, sizeof(value));
  }

  /** @brief 指定发送 IPv6 组播使用的接口索引。 */
  auto set_multicast_interface_v6(std::uint32_t network_interface) noexcept -> expected<void> {
    return set_option(
        IPPROTO_IPV6, IPV6_MULTICAST_IF, &network_interface, sizeof(network_interface));
  }

  /**
   *  @brief 启用 recv_message 的目标地址/接口辅助数据，保留平台原生 cmsghdr
   * 格式。
   */
  auto set_recv_packet_info_v4(bool on) noexcept -> expected<void> {
    return set_optional_integer(IPPROTO_IP, packet_info_v4_option, on);
  }

  /** @brief 设置接收 IPv6 数据报时是否附带目标和接口信息。 */
  auto set_recv_packet_info_v6(bool on) noexcept -> expected<void> {
    return set_optional_integer(IPPROTO_IPV6, packet_info_v6_option, on);
  }

  /** @brief 设置 Unix 数据报接收是否附带发送方凭据。 */
  auto set_passcred(bool on) noexcept -> expected<void> {
    return set_optional_integer(SOL_SOCKET, passcred_option, on);
  }

  /** @brief 查询 Unix 数据报凭据传递设置。 */
  [[nodiscard]] auto passcred() const noexcept -> expected<bool> {
    return optional_boolean(SOL_SOCKET, passcred_option);
  }

  /** @brief 设置 Linux socket 的数据包标记。 */
  auto set_mark(std::uint32_t mark) noexcept -> expected<void> {
    if (!mark_option)
      return std::unexpected{make_error(ENOTSUP)};
    return set_option(SOL_SOCKET, *mark_option, &mark, sizeof(mark));
  }
};

/** @brief 保留旧 CRTP 名称以兼容源码；新对象统一继承同一个选项 mixin。 */
template <class T>
using ImplNodelay = ImplSocketOptions<T>;
template <class T>
using ImplPasscred = ImplSocketOptions<T>;
template <class T>
using ImplRecvBufSize = ImplSocketOptions<T>;
template <class T>
using ImplSendBufSize = ImplSocketOptions<T>;
template <class T>
using ImplKeepalive = ImplSocketOptions<T>;
template <class T>
using ImplLinger = ImplSocketOptions<T>;
template <class T>
using ImplBoradcast = ImplSocketOptions<T>;
template <class T>
using ImplTTL = ImplSocketOptions<T>;
template <class T>
using ImplReuseAddr = ImplSocketOptions<T>;
template <class T>
using ImplReusePort = ImplSocketOptions<T>;
template <class T>
using ImplMark = ImplSocketOptions<T>;
}  // namespace faio::net::detail
#endif
