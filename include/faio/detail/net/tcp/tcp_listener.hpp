#ifndef FAIO_DETAIL_NET_TCP_TCP_LISTENER_HPP
#define FAIO_DETAIL_NET_TCP_TCP_LISTENER_HPP
#include "faio/detail/net/tcp/base_listener.hpp"
#include "faio/detail/net/tcp/tcp_stream.hpp"
namespace faio::net::detail {
class TcpListener
    : public detail::BaseListener<TcpListener, TcpStream, SocketAddr>,
      public detail::ImplTTL<TcpListener> {
public:
  explicit TcpListener(detail::Socket &&inner)
      : detail::BaseListener<TcpListener, TcpStream, SocketAddr>{
            std::move(inner)} {}
};
} // namespace faio::net::detail
#endif // FAIO_DETAIL_NET_TCP_TCP_LISTENER_HPP