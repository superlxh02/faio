/**
 * @file faio/detail/net/udp/socket.hpp
 * @brief 套接字资源封装。
 */
#ifndef FAIO_DETAIL_NET_UDP_SOCKET_HPP
#define FAIO_DETAIL_NET_UDP_SOCKET_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/io/awaiter/connect.hpp"
#include "faio/detail/net/base/address_value.hpp"
#include "faio/detail/net/resolver.hpp"
#include "faio/detail/net/socket_options.hpp"
#include "faio/detail/net/base/datagram.hpp"

namespace faio::net::detail {
/**
 *  @brief IPv4/IPv6 UDP
 * socket，提供连接式/无连接式收发、消息控制数据与组播配置。
 */
class udp_socket : public base_datagram<udp_socket, socket_addr>,
                   public impl_socket_options<udp_socket> {
 public:
  using base_datagram::bind;
  using base_datagram::connect;

  /** @brief 接管源对象的状态和资源租约。 */
  explicit udp_socket(socket&& socket) : base_datagram{std::move(socket)} {}

  /**
   *  @brief 同步创建未绑定的 UDP socket，返回
   * expected，供后续配置与异步连接使用。
   */
  [[nodiscard]] static auto unbound(bool ipv6 = false,
                                    io::io_context context = io::io_context::current())
      -> expected<udp_socket> {
    return socket::create<udp_socket>(
        ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, std::move(context));
  }

  /** @brief 创建未绑定的数据报 socket 并关联指定 IO 上下文。 */
  [[nodiscard]] static auto unbound(io::io_context context, bool ipv6 = false)
      -> expected<udp_socket> {
    return unbound(ipv6, std::move(context));
  }

  /**  @brief 异步解析 host/service，然后调用数字地址的同步创建和绑定接口。 */
  static auto bind(io::io_context context, host_port endpoint) -> task<expected<udp_socket>> {
    auto addresses = co_await lookup_host(
        context, std::move(endpoint.host), std::move(endpoint.service), SOCK_DGRAM);
    if (!addresses)
      co_return std::unexpected{addresses.error()};
    co_return base_datagram::bind(*addresses, std::move(context));
  }

  /** @brief 连接到指定对端，返回连接结果。 */
  auto connect(host_port endpoint) const -> task<expected<void>> {
    return connect_host(resource(), context(), std::move(endpoint));
  }

 private:
  /** @brief 解析候选端点并依次尝试连接当前数据报 socket。 */
  static auto connect_host(std::shared_ptr<io::detail::resource_state> resource,
                           io::io_context context,
                           host_port endpoint) -> task<expected<void>> {
    auto addresses = co_await lookup_host(
        std::move(context), std::move(endpoint.host), std::move(endpoint.service), SOCK_DGRAM);
    if (!addresses)
      co_return std::unexpected{addresses.error()};
    error last{::faio::error::INVALID_ADDRESSES};
    for (const auto& address : *addresses) {
      auto result = co_await io::connect(resource, address.sockaddr(), address.length());
      if (result)
        co_return result;
      last = result.error();
    }
    co_return std::unexpected{last};
  }
};

/**  @brief UdpSocket 的兼容名称。 */
using udp_datagram = udp_socket;
}  // namespace faio::net::detail
#endif
