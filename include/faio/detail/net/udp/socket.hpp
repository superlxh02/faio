/**
 * @file faio/detail/net/udp/socket.hpp
 * @brief 网络地址、socket 与传输协议：socket 资源封装。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#ifndef FAIO_DETAIL_NET_UDP_SOCKET_HPP
#define FAIO_DETAIL_NET_UDP_SOCKET_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/net/base/address_value.hpp"
#include "faio/detail/net/resolver.hpp"
#include "faio/detail/net/socket_options.hpp"
#include "faio/detail/net/base/datagram.hpp"

namespace faio::net::detail {
/** @brief IPv4/IPv6 UDP
 * socket，提供连接式/无连接式收发、消息控制数据与组播配置。 */
class UdpSocket : public BaseDatagram<UdpSocket, SocketAddr>, public ImplSocketOptions<UdpSocket> {
 public:
  using BaseDatagram::bind;
  using BaseDatagram::connect;

  explicit UdpSocket(Socket&& socket) : BaseDatagram{std::move(socket)} {}

  /** @brief 同步创建未绑定的 UDP socket，返回
   * expected，供后续配置与异步连接使用。 */
  [[nodiscard]] static auto unbound(bool ipv6 = false,
                                    io::io_context context = io::io_context::current())
      -> expected<UdpSocket> {
    return Socket::create<UdpSocket>(
        ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, std::move(context));
  }

  [[nodiscard]] static auto unbound(io::io_context context, bool ipv6 = false)
      -> expected<UdpSocket> {
    return unbound(ipv6, std::move(context));
  }

  /** @brief 异步解析 host/service，然后调用数字地址的同步创建和绑定接口。 */
  static auto bind(io::io_context context, HostPort endpoint) -> task<expected<UdpSocket>> {
    auto addresses = co_await lookup_host(
        context, std::move(endpoint.host), std::move(endpoint.service), SOCK_DGRAM);
    if (!addresses)
      co_return std::unexpected{addresses.error()};
    co_return BaseDatagram::bind(*addresses, std::move(context));
  }

  auto connect(HostPort endpoint) const -> task<expected<void>> {
    return connect_host(resource(), context(), std::move(endpoint));
  }

 private:
  static auto connect_host(std::shared_ptr<io::detail::resource_state> resource,
                           io::io_context context,
                           HostPort endpoint) -> task<expected<void>> {
    auto addresses = co_await lookup_host(
        std::move(context), std::move(endpoint.host), std::move(endpoint.service), SOCK_DGRAM);
    if (!addresses)
      co_return std::unexpected{addresses.error()};
    Error last{Error::InvalidAddresses};
    for (const auto& address : *addresses) {
      auto result = co_await io::connect(resource, address.sockaddr(), address.length());
      if (result)
        co_return result;
      last = result.error();
    }
    co_return std::unexpected{last};
  }
};

/** @brief 旧数据报名称保留源码兼容，完整接口集中于 UdpSocket。 */
using UdpDatagram = UdpSocket;
}
#endif
