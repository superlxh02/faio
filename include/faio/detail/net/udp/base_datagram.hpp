#ifndef FAIO_DETAIL_NET_UDP_BASE_DATAGRAM_HPP
#define FAIO_DETAIL_NET_UDP_BASE_DATAGRAM_HPP
#include "faio/detail/net/common/addr_util.hpp"
#include "faio/detail/net/common/datagram_recv.hpp"
#include "faio/detail/net/common/datagram_send.hpp"
#include "faio/detail/net/common/socket.hpp"
namespace faio::net::detail {
template <class Datagram, class Addr>
class BaseDatagram : public ImplSend<BaseDatagram<Datagram, Addr>, Addr>,
                     public ImplRecv<BaseDatagram<Datagram, Addr>, Addr>,
                     public ImplLocalAddr<BaseDatagram<Datagram, Addr>, Addr>,
                     public ImplPeerAddr<BaseDatagram<Datagram, Addr>, Addr> {
protected:
  explicit BaseDatagram(Socket &&inner) : _inner_socket{std::move(inner)} {}

public:
  auto connect(const Addr &addr) noexcept {
    return io::detail::Connect{fd(), addr.sockaddr(), addr.length()};
  }

  auto close() noexcept { return _inner_socket.close(); }

  [[nodiscard]]
  auto fd() const noexcept {
    return _inner_socket.fd();
  }

public:
  [[nodiscard]]
  static auto bind(const Addr &addr) -> expected<Datagram> {
    auto ret =
        Socket::create<Datagram>(addr.family(), SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (!ret) {
      return std::unexpected{ret.error()};
    }
    if (::bind(ret.value().fd(), addr.sockaddr(), addr.length()) != 0) {
      return std::unexpected{make_error(errno)};
    }
    return Datagram{std::move(ret.value())};
  }

private:
  Socket _inner_socket;
};
} // namespace faio::net::detail
#endif // FAIO_DETAIL_NET_UDP_BASE_DATAGRAM_HPP