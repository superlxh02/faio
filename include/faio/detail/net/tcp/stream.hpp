/**
 * @file faio/detail/net/tcp/stream.hpp
 * @brief 网络地址、socket 与传输协议：字节流公共封装。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#ifndef FAIO_DETAIL_NET_TCP_STREAM_HPP
#define FAIO_DETAIL_NET_TCP_STREAM_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/net/base/address_value.hpp"
#include "faio/detail/net/resolver.hpp"
#include "faio/detail/net/base/stream.hpp"
#include "faio/detail/net/base/halves.hpp"
#include <tuple>

namespace faio::net::detail {
/** @brief 完整双工 TCP 字节流，读写方向分别由底层资源控制块串行化。 */
class TcpStream : public BaseStream<TcpStream, SocketAddr>, public ImplSocketOptions<TcpStream> {
 public:
  using BaseStream::connect;
  using ReadHalf = BasicReadHalf<TcpStream, SocketAddr, false>;  ///< 借用式读半部类型，底层流须覆盖借用期。
  using WriteHalf = BasicWriteHalf<TcpStream, SocketAddr, false>;  ///< 借用式写半部类型，底层流须覆盖借用期。
  using OwnedReadHalf = BasicReadHalf<TcpStream, SocketAddr, true>;  ///< 持有资源租约的拥有式读半部类型。
  using OwnedWriteHalf = BasicWriteHalf<TcpStream, SocketAddr, true>;  ///< 持有资源租约的拥有式写半部类型。

  explicit TcpStream(Socket&& socket) : BaseStream{std::move(socket)} {}

  /** @brief 主机名连接在 resolver lane 异步解析，依输入次序尝试全部地址。 */
  static auto connect(io::io_context context, HostPort endpoint) -> task<expected<TcpStream>> {
    auto addresses =
        co_await lookup_host(context, std::move(endpoint.host), std::move(endpoint.service));
    if (!addresses)
      co_return std::unexpected{addresses.error()};
    co_return co_await BaseStream::connect(*addresses, std::move(context));
  }

  static auto connect(HostPort endpoint, io::io_context context = io::io_context::current())
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
  [[nodiscard]] auto into_split() && -> std::pair<OwnedReadHalf, OwnedWriteHalf> {
    auto identity = std::make_shared<char>();
    Socket socket = take_socket();
    Socket second = socket.share();
    return {std::piecewise_construct,
            std::forward_as_tuple(std::move(socket), identity),
            std::forward_as_tuple(std::move(second), identity)};
  }
};
}
#endif
