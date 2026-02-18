#ifndef FAIO_DETAIL_NET_TCP_TCP_STREAM_HPP
#define FAIO_DETAIL_NET_TCP_TCP_STREAM_HPP

#include "faio/detail/net/common/address.hpp"
#include "faio/detail/net/tcp/base_stream.hpp"
namespace faio::net::detail {
class TcpStream : public detail::BaseStream<TcpStream, SocketAddr>,
                  public detail::ImplNodelay<TcpStream>,
                  public detail::ImplLinger<TcpStream>,
                  public detail::ImplTTL<TcpStream> {
public:
  explicit TcpStream(detail::Socket &&inner) : BaseStream{std::move(inner)} {}

public:
};
} // namespace faio::net::detail
#endif // FAIO_DETAIL_NET_TCP_TCP_STREAM_HPP