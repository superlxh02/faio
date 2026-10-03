#ifndef FAIO_DETAIL_NET_TCP_TCP_STREAM_HPP
#define FAIO_DETAIL_NET_TCP_TCP_STREAM_HPP
#include "faio/detail/net/common/address.hpp"
#include "faio/detail/net/common/resolver.hpp"
#include "faio/detail/net/tcp/base_stream.hpp"
#include "faio/detail/net/tcp/halves.hpp"
#include <tuple>
namespace faio::net::detail {
/** @brief 完整双工 TCP 字节流，读写方向分别由底层资源控制块串行化。 */
class TcpStream : public BaseStream<TcpStream, SocketAddr>,
                  public ImplSocketOptions<TcpStream> {
public:
  using BaseStream::connect;
  using ReadHalf = BasicReadHalf<TcpStream, SocketAddr, false>;
  using WriteHalf = BasicWriteHalf<TcpStream, SocketAddr, false>;
  using OwnedReadHalf = BasicReadHalf<TcpStream, SocketAddr, true>;
  using OwnedWriteHalf = BasicWriteHalf<TcpStream, SocketAddr, true>;
  explicit TcpStream(Socket &&socket) : BaseStream{std::move(socket)} {}
  /** @brief 主机名连接在 resolver lane 异步解析，依输入次序尝试全部地址。 */
  static auto connect(io::io_context context, HostPort endpoint)
      -> task<expected<TcpStream>> {
    auto addresses = co_await lookup_host(context, std::move(endpoint.host),
                                          std::move(endpoint.service));
    if (!addresses)
      co_return std::unexpected{addresses.error()};
    co_return co_await BaseStream::connect(*addresses, std::move(context));
  }
  static auto connect(HostPort endpoint,
                      io::io_context context = io::io_context::current())
      -> task<expected<TcpStream>> {
    co_return co_await connect(std::move(context), std::move(endpoint));
  }
  /** @brief 创建安全借用半边，父 stream 与半边同时保持同一资源。 */
  [[nodiscard]] auto split() & -> std::pair<ReadHalf, WriteHalf> {
    auto identity = std::make_shared<char>();
    return {std::piecewise_construct,
            std::forward_as_tuple(share_socket(), identity),
            std::forward_as_tuple(share_socket(), identity)};
  }
  /** @brief 消费 stream，两个半边可分别转移给不同协程。 */
  [[nodiscard]] auto
  into_split() && -> std::pair<OwnedReadHalf, OwnedWriteHalf> {
    auto identity = std::make_shared<char>();
    Socket socket = take_socket();
    Socket second = socket.share();
    return {std::piecewise_construct,
            std::forward_as_tuple(std::move(socket), identity),
            std::forward_as_tuple(std::move(second), identity)};
  }
};
} // namespace faio::net::detail
#endif
