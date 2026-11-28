/**
 * @file faio/detail/net/tcp/stream.hpp
 * @brief 字节流公共封装。
 */
#ifndef FAIO_DETAIL_NET_TCP_STREAM_HPP
#define FAIO_DETAIL_NET_TCP_STREAM_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/net/socket_options.hpp"
#include "faio/detail/net/base/address_value.hpp"
#include "faio/detail/net/resolver.hpp"
#include "faio/detail/net/base/stream.hpp"
#include "faio/detail/net/base/halves.hpp"
#include <tuple>

namespace faio::net::detail {
/**  @brief 全双工 TCP 字节流，读写方向分别由底层资源控制块串行化。 */
class tcp_stream : public base_stream<tcp_stream, socket_addr>,
                   public impl_socket_options<tcp_stream> {
 public:
  using base_stream::connect;
  using read_half = basic_read_half<tcp_stream,
                                    socket_addr,
                                    false>; /**< 借用式读半部类型，底层流须在整个借用期内存活。 */
  using write_half = basic_write_half<tcp_stream,
                                      socket_addr,
                                      false>; /**< 借用式写半部类型，底层流须在整个借用期内存活。 */
  using owned_read_half =
      basic_read_half<tcp_stream, socket_addr, true>; /**< 持有资源租约的拥有式读半部类型。 */
  using owned_write_half =
      basic_write_half<tcp_stream, socket_addr, true>; /**< 持有资源租约的拥有式写半部类型。 */

  /** @brief 接管源对象的状态和资源租约。 */
  explicit tcp_stream(socket&& socket) : base_stream{std::move(socket)} {}

  /**  @brief 主机名连接在 resolver lane 异步解析，依输入次序尝试全部地址。 */
  static auto connect(io::io_context context, host_port endpoint) -> task<expected<tcp_stream>> {
    auto addresses =
        co_await lookup_host(context, std::move(endpoint.host), std::move(endpoint.service));
    if (!addresses)
      co_return std::unexpected{addresses.error()};
    co_return co_await base_stream::connect(*addresses, std::move(context));
  }

  /** @brief 连接到指定对端，返回连接结果。 */
  static auto connect(host_port endpoint, io::io_context context = io::io_context::current())
      -> task<expected<tcp_stream>> {
    co_return co_await connect(std::move(context), std::move(endpoint));
  }

  /**  @brief 创建安全借用半部，父 stream 与半部同时保持同一资源。 */
  [[nodiscard]] auto split() & -> std::pair<read_half, write_half> {
    auto identity = std::make_shared<char>();
    return {std::piecewise_construct,
            std::forward_as_tuple(share_socket(), identity),
            std::forward_as_tuple(share_socket(), identity)};
  }

  /**  @brief 消费 stream，两个半部可分别转移给不同协程。 */
  [[nodiscard]] auto into_split() && -> std::pair<owned_read_half, owned_write_half> {
    auto identity = std::make_shared<char>();
    socket first = take_socket();
    socket second = first.share();
    return {std::piecewise_construct,
            std::forward_as_tuple(std::move(first), identity),
            std::forward_as_tuple(std::move(second), identity)};
  }
};
}  // namespace faio::net::detail
#endif
