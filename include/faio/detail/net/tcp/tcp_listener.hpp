#ifndef FAIO_DETAIL_NET_TCP_TCP_LISTENER_HPP
#define FAIO_DETAIL_NET_TCP_TCP_LISTENER_HPP
#include "faio/detail/net/common/platform.hpp"
#include "faio/detail/net/tcp/base_listener.hpp"
#include "faio/detail/net/tcp/tcp_stream.hpp"

namespace faio::net::detail {
/** @brief TCP 监听器，bind/accept/close 与后端选择无关。 */
class TcpListener : public BaseListener<TcpListener, TcpStream, SocketAddr>,
                    public ImplSocketOptions<TcpListener> {
 public:
  using BaseListener::bind;

  explicit TcpListener(Socket&& socket) : BaseListener{std::move(socket)} {}

  /** @brief 主机名绑定先经独立 resolver 解析，再逐地址同步创建、bind 和
   * listen。
   * @details 返回组合 task；最终监听器与数字端点的同步 expected 接口行为一致。
   */
  static auto bind(io::io_context context, HostPort endpoint, int backlog = SOMAXCONN)
      -> task<expected<TcpListener>> {
    auto addresses =
        co_await lookup_host(context, std::move(endpoint.host), std::move(endpoint.service));
    if (!addresses)
      co_return std::unexpected{addresses.error()};
    Error last{Error::InvalidAddresses};
    for (const auto& address : *addresses) {
      auto result = BaseListener::bind(address, context, backlog);
      if (result)
        co_return result;
      last = result.error();
    }
    co_return std::unexpected{last};
  }
};
}  // namespace faio::net::detail
#endif
