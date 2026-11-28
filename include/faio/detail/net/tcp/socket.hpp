/**
 * @file faio/detail/net/tcp/socket.hpp
 * @brief 套接字资源封装。
 */
#ifndef FAIO_DETAIL_NET_TCP_SOCKET_HPP
#define FAIO_DETAIL_NET_TCP_SOCKET_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/net/socket_options.hpp"
#include "faio/detail/io/awaiter/connect.hpp"
#include "faio/detail/net/tcp/listener.hpp"

namespace faio::net::detail {
/**  @brief 可先配置选项、绑定本地地址再 connect/listen 的独占 TCP socket。 */
class tcp_socket : public impl_socket_options<tcp_socket>,
                   public impl_local_addr<tcp_socket, socket_addr>,
                   public impl_peer_addr<tcp_socket, socket_addr> {
 public:
  /** @brief 接管源对象的状态和资源租约。 */
  explicit tcp_socket(socket&& socket) : socket_{std::move(socket)} {}

  /** @brief 接管源对象的状态和资源租约。 */
  tcp_socket(tcp_socket&&) noexcept = default;

  /** @brief 移动赋值资源及状态。 */
  auto operator=(tcp_socket&&) noexcept -> tcp_socket& = default;

  /** @brief 禁止复制此对象。 */
  tcp_socket(const tcp_socket&) = delete;

  /** @brief 禁止复制赋值。 */
  auto operator=(const tcp_socket&) -> tcp_socket& = delete;

  /** @brief 释放 TCP socket 的资源租约。 */
  ~tcp_socket() = default;

  /**
   *  @brief 同步创建 IPv4 配置 socket，返回 expected；后续 connect
   * 才提交连接请求。
   * @details 本接口使用短 socket
   * syscall，保留调用方先配置选项与本地地址的约定。
   */
  [[nodiscard]] static auto new_v4(io::io_context context = io::io_context::current())
      -> expected<tcp_socket> {
    return socket::create<tcp_socket>(AF_INET, SOCK_STREAM, IPPROTO_TCP, std::move(context));
  }

  /**  @brief 同步创建 IPv6 配置 socket，返回 expected，设置非阻塞及 CLOEXEC。 */
  [[nodiscard]] static auto new_v6(io::io_context context = io::io_context::current())
      -> expected<tcp_socket> {
    return socket::create<tcp_socket>(AF_INET6, SOCK_STREAM, IPPROTO_TCP, std::move(context));
  }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto fd() const noexcept -> native_socket_type { return socket_.fd(); }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto as_native_handle() const noexcept -> native_socket_type { return fd(); }

  /** @brief 取得该对象关联的 I/O 上下文租约。 */
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }

  /** @brief 取得底层共享资源控制块，保持句柄身份及所属域有效。 */
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }

  /** @brief 将 socket 绑定到给定本地地址。 */
  [[nodiscard]] auto bind(const socket_addr& address) -> expected<void> {
    return socket_.bind(address);
  }

  /**  @brief listen 消费 socket 状态；失败仍保持原 socket 可配置。 */
  [[nodiscard]] auto listen(int backlog = SOMAXCONN) && -> expected<tcp_listener> {
    if (auto result = socket_.listen(backlog); !result)
      return std::unexpected{result.error()};
    return tcp_listener{std::move(socket_)};
  }

  /**
   *  @brief 立即将已配置 socket 移入组合协程帧，返回
   * task<expected<TcpStream>>。
   * @details 父对象销毁不影响在途连接；io_uring 对已有 fd 提交 CONNECT，
   *          不重新创建 socket。静态 TcpStream::connect 另负责异步创建句柄。
   */
  auto connect(socket_addr address) && -> task<expected<tcp_stream>> {
    return connect_owned(std::move(socket_), std::move(address));
  }

  /** @brief 异步关闭共享套接字，等待在途操作安全结束。 */
  auto close() noexcept { return socket_.close(); }

  /** @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。 */
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }

  /** @brief 接管显式传入的原生句柄，按平台规则建立资源包装。 */
  [[nodiscard]] static auto from_native(io::io_context context, owned_native_socket native)
      -> expected<tcp_socket> {
    if (auto placement = native.validate_import(context); !placement)
      return std::unexpected{placement.error()};
    int type{};
    socklen_t length = sizeof(type);
    if (socket_getsockopt(native.get(), SOL_SOCKET, SO_TYPE, &type, &length) < 0)
      return std::unexpected{socket_error()};
    if (type != SOCK_STREAM)
      return std::unexpected{make_error(::faio::error::INVALID_SOCKET_TYPE)};
    if (auto valid = socket::validate_family<socket_addr>(native.get()); !valid)
      return std::unexpected{valid.error()};
    if (auto result = socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    socket::release_imported(native);
    return tcp_socket{std::move(*socket)};
  }

 private:
  /** @brief 持有 socket 直至连接完成，再交付连接流或错误。 */
  static auto connect_owned(socket socket, socket_addr address) -> task<expected<tcp_stream>> {
    auto result = co_await io::connect(socket.resource(), address.sockaddr(), address.length());
    if (!result)
      co_return std::unexpected{result.error()};
    co_return tcp_stream{std::move(socket)};
  }

  socket socket_; /**< 持有或共享底层 socket 包装，在挂起期间保持原生资源身份有效。 */
};
}  // namespace faio::net::detail
#endif
