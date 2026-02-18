#ifndef FAIO_DETAIL_NET_UDP_DATAGRAM_HPP
#define FAIO_DETAIL_NET_UDP_DATAGRAM_HPP
#include "faio/detail/net/common/address.hpp"
#include "faio/detail/net/common/sockopt.hpp"
#include "faio/detail/net/udp/base_datagram.hpp"
namespace faio::net::detail {
class UdpDatagram : public detail::BaseDatagram<UdpDatagram, SocketAddr>,
                    public detail::ImplBoradcast<UdpDatagram>,
                    public detail::ImplTTL<UdpDatagram> {

public:
  explicit UdpDatagram(detail::Socket &&inner)
      : BaseDatagram{std::move(inner)} {}

public:
  [[nodiscard]]
  static auto unbound(bool is_ipv6 = false) -> expected<UdpDatagram> {
    return detail::Socket::create<UdpDatagram>(
        is_ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
  }
};
} // namespace faio::net::detail
#endif // FAIO_DETAIL_NET_UDP_DATAGRAM_HPP