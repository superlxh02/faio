/**
 * @file faio/detail/net/tcp/socket.hpp
 * @brief 网络地址、socket 与传输协议：socket 资源封装。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#ifndef FAIO_DETAIL_NET_TCP_SOCKET_HPP
#define FAIO_DETAIL_NET_TCP_SOCKET_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/net/tcp/listener.hpp"

namespace faio::net::detail {
/** @brief 可先配置选项、绑定本地地址再 connect/listen 的独占 TCP socket。 */
class TcpSocket : public ImplSocketOptions<TcpSocket>,
                  public ImplLocalAddr<TcpSocket, SocketAddr>,
                  public ImplPeerAddr<TcpSocket, SocketAddr> {
 public:
  explicit TcpSocket(Socket&& socket) : socket_{std::move(socket)} {}

  TcpSocket(TcpSocket&&) noexcept = default;

  auto operator=(TcpSocket&&) noexcept -> TcpSocket& = default;

  TcpSocket(const TcpSocket&) = delete;

  auto operator=(const TcpSocket&) -> TcpSocket& = delete;

  ~TcpSocket() = default;

  /** @brief 同步创建 IPv4 配置 socket，返回 expected；后续 connect
   * 才提交连接请求。
   * @details 本接口使用短 socket
   * syscall，保留调用方先配置选项与本地地址的合同。
   */
  [[nodiscard]] static auto new_v4(io::io_context context = io::io_context::current())
      -> expected<TcpSocket> {
    return Socket::create<TcpSocket>(AF_INET, SOCK_STREAM, IPPROTO_TCP, std::move(context));
  }

  /** @brief 同步创建 IPv6 配置 socket，返回 expected，设置非阻塞及 CLOEXEC。 */
  [[nodiscard]] static auto new_v6(io::io_context context = io::io_context::current())
      -> expected<TcpSocket> {
    return Socket::create<TcpSocket>(AF_INET6, SOCK_STREAM, IPPROTO_TCP, std::move(context));
  }

  /// @brief 读取原生句柄值，不在此接口转移关闭责任。
  [[nodiscard]] auto fd() const noexcept -> native_socket_type { return socket_.fd(); }

  /// @brief 读取原生句柄值，不在此接口转移关闭责任。
  [[nodiscard]] auto as_native_handle() const noexcept -> native_socket_type { return fd(); }

  /// @brief 取得当前对象关联的 IO 上下文租约。
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }

  /// @brief 取得底层共享资源控制块，以保持句柄身份与所属域。
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }

  [[nodiscard]] auto bind(const SocketAddr& address) -> expected<void> {
    return socket_.bind(address);
  }

  /** @brief listen 消费 socket 状态；失败仍保持原 socket 可配置。 */
  [[nodiscard]] auto listen(int backlog = SOMAXCONN) && -> expected<TcpListener> {
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

  /// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
  auto close() noexcept { return socket_.close(); }

  /// @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }

  /// @brief 接管显式传入的原生句柄，按平台规则建立资源包装。
  [[nodiscard]] static auto from_native(io::io_context context, owned_native_socket native)
      -> expected<TcpSocket> {
    if (auto placement = native.validate_import(context); !placement)
      return std::unexpected{placement.error()};
    int type{};
    socklen_t length = sizeof(type);
    if (socket_getsockopt(native.get(), SOL_SOCKET, SO_TYPE, &type, &length) < 0)
      return std::unexpected{socket_error()};
    if (type != SOCK_STREAM)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    if (auto valid = Socket::validate_family<SocketAddr>(native.get()); !valid)
      return std::unexpected{valid.error()};
    if (auto result = Socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = Socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    Socket::release_imported(native);
    return TcpSocket{std::move(*socket)};
  }

 private:
  static auto connect_owned(Socket socket, SocketAddr address) -> task<expected<TcpStream>> {
    auto result = co_await io::connect(socket.resource(), address.sockaddr(), address.length());
    if (!result)
      co_return std::unexpected{result.error()};
    co_return TcpStream{std::move(socket)};
  }

  Socket socket_;  ///< 拥有或共享底层 socket 包装，跨挂起保持原生资源身份。
};
}
#endif
