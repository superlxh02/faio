#ifndef FAIO_DETAIL_NET_TCP_SOCKET_HPP
#define FAIO_DETAIL_NET_TCP_SOCKET_HPP
#include "faio/detail/net/tcp/tcp_listener.hpp"
namespace faio::net::detail {
/** @brief 可先配置选项、绑定本地地址再 connect/listen 的独占 TCP socket。 */
class TcpSocket : public ImplSocketOptions<TcpSocket>,
                  public ImplLocalAddr<TcpSocket, SocketAddr>,
                  public ImplPeerAddr<TcpSocket, SocketAddr> {
public:
  explicit TcpSocket(Socket &&socket) : socket_{std::move(socket)} {}
  TcpSocket(TcpSocket &&) noexcept = default;
  auto operator=(TcpSocket &&) noexcept -> TcpSocket & = default;
  TcpSocket(const TcpSocket &) = delete;
  auto operator=(const TcpSocket &) -> TcpSocket & = delete;
  ~TcpSocket() = default;
  /** @brief 同步创建 IPv4 配置 socket，返回 expected；后续 connect
   * 才提交连接请求。
   * @details 本接口使用短 socket
   * syscall，保留调用方先配置选项与本地地址的合同。
   */
  [[nodiscard]] static auto
  new_v4(io::io_context context = io::io_context::current())
      -> expected<TcpSocket> {
    return Socket::create<TcpSocket>(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                     std::move(context));
  }
  /** @brief 同步创建 IPv6 配置 socket，返回 expected，设置非阻塞及 CLOEXEC。 */
  [[nodiscard]] static auto
  new_v6(io::io_context context = io::io_context::current())
      -> expected<TcpSocket> {
    return Socket::create<TcpSocket>(AF_INET6, SOCK_STREAM, IPPROTO_TCP,
                                     std::move(context));
  }
  [[nodiscard]] auto fd() const noexcept -> int { return socket_.fd(); }
  [[nodiscard]] auto as_native_handle() const noexcept -> int { return fd(); }
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }
  [[nodiscard]] auto bind(const SocketAddr &address) -> expected<void> {
    return socket_.bind(address);
  }
  /** @brief listen 消费 socket 状态；失败仍保持原 socket 可配置。 */
  [[nodiscard]] auto
  listen(int backlog = SOMAXCONN) && -> expected<TcpListener> {
    if (auto result = socket_.listen(backlog); !result)
      return std::unexpected{result.error()};
    return TcpListener{std::move(socket_)};
  }
  /** @brief 立刻消费已配置 socket 到组合协程帧，返回
   * task<expected<TcpStream>>。
   * @details 父对象销毁不影响在途连接；io_uring 对已有 fd 提交 CONNECT，
   *          不重新创建 socket。静态 TcpStream::connect 另负责异步创建句柄。
   */
  auto connect(SocketAddr address) && -> task<expected<TcpStream>> {
    return connect_owned(std::move(socket_), std::move(address));
  }
  auto close() noexcept { return socket_.close(); }
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }
  [[nodiscard]] static auto from_native(io::io_context context,
                                        owned_native_socket native)
      -> expected<TcpSocket> {
    int type{};
    socklen_t length = sizeof(type);
    if (::getsockopt(native.get(), SOL_SOCKET, SO_TYPE, &type, &length) < 0)
      return std::unexpected{make_error(errno)};
    if (type != SOCK_STREAM)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    if (auto valid = Socket::validate_family<SocketAddr>(native.get()); !valid)
      return std::unexpected{valid.error()};
    if (auto result = Socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = Socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return TcpSocket{std::move(*socket)};
  }

private:
  static auto connect_owned(Socket socket, SocketAddr address)
      -> task<expected<TcpStream>> {
    auto result = co_await io::connect(socket.resource(), address.sockaddr(),
                                       address.length());
    if (!result)
      co_return std::unexpected{result.error()};
    co_return TcpStream{std::move(socket)};
  }
  Socket socket_;
};
} // namespace faio::net::detail
#endif
