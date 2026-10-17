/**
 * @file faio/detail/net/unix_domain/socket.hpp
 * @brief 网络地址、socket 与传输协议：socket 资源封装。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#ifndef FAIO_DETAIL_NET_UNIX_DOMAIN_SOCKET_HPP
#define FAIO_DETAIL_NET_UNIX_DOMAIN_SOCKET_HPP
#include "faio/detail/net/socket_options.hpp"
#include "faio/detail/net/base/listener.hpp"
#include "faio/detail/net/base/stream.hpp"
#include "faio/detail/net/base/halves.hpp"
#include "faio/detail/net/base/datagram.hpp"
#include "faio/detail/net/unix_domain/address.hpp"
#include <tuple>

namespace faio::net::unix::detail {
using Socket = ::faio::net::detail::Socket;
using owned_native_socket = ::faio::net::detail::owned_native_socket;

/** @brief 对端身份，macOS 的 pid 不由 getpeereid 提供，因此为 nullopt。 */
struct PeerCredentials {
  std::uint32_t uid{};  ///< Unix 对端用户身份编号。
  std::uint32_t gid{};  ///< Unix 对端用户组身份编号。
  std::optional<std::int64_t> pid;  ///< 平台可提供时记录的 Unix 对端进程编号。
};

class UnixStream : public ::faio::net::detail::BaseStream<UnixStream, UnixAddr>,
                   public ::faio::net::detail::ImplSocketOptions<UnixStream> {
 public:
  using ReadHalf = ::faio::net::detail::BasicReadHalf<UnixStream, UnixAddr, false>;  ///< 借用式读半部类型，底层流须覆盖借用期。
  using WriteHalf = ::faio::net::detail::BasicWriteHalf<UnixStream, UnixAddr, false>;  ///< 借用式写半部类型，底层流须覆盖借用期。
  using OwnedReadHalf = ::faio::net::detail::BasicReadHalf<UnixStream, UnixAddr, true>;  ///< 持有资源租约的拥有式读半部类型。
  using OwnedWriteHalf = ::faio::net::detail::BasicWriteHalf<UnixStream, UnixAddr, true>;  ///< 持有资源租约的拥有式写半部类型。

  explicit UnixStream(Socket&& socket) : BaseStream{std::move(socket)} {}

  [[nodiscard]] auto split() & -> std::pair<ReadHalf, WriteHalf> {
    auto identity = std::make_shared<char>();
    return {std::piecewise_construct,
            std::forward_as_tuple(share_socket(), identity),
            std::forward_as_tuple(share_socket(), identity)};
  }

  [[nodiscard]] auto into_split() && -> std::pair<OwnedReadHalf, OwnedWriteHalf> {
    auto identity = std::make_shared<char>();
    Socket first = take_socket();
    Socket second = first.share();
    return {std::piecewise_construct,
            std::forward_as_tuple(std::move(first), identity),
            std::forward_as_tuple(std::move(second), identity)};
  }

  /** @brief 创建非阻塞全双工 socketpair，失败时两个原生 fd 都由 RAII 归还。 */
  [[nodiscard]] static auto pair(io::io_context context = io::io_context::current())
      -> expected<std::pair<UnixStream, UnixStream>> {
    int descriptors[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) < 0)
      return std::unexpected{make_error(errno)};
    owned_native_socket first{descriptors[0]}, second{descriptors[1]};
    if (auto prepared = Socket::prepare(first.get()); !prepared)
      return std::unexpected{prepared.error()};
    if (auto prepared = Socket::prepare(second.get()); !prepared)
      return std::unexpected{prepared.error()};
    auto a = Socket::adopt_checked(first.get(), context);
    if (!a)
      return std::unexpected{a.error()};
    (void)first.release();
    auto b = Socket::adopt_checked(second.get(), context);
    if (!b)
      return std::unexpected{b.error()};
    (void)second.release();
    return std::pair{UnixStream{std::move(*a)}, UnixStream{std::move(*b)}};
  }

  /** @brief 查询已连接 Unix socket 对端的内核身份信息。 */
  [[nodiscard]] auto peer_credentials() const noexcept -> expected<PeerCredentials> {
    return io::detail::with_resource(
        resource(), io::Interest::none, [&]() -> expected<PeerCredentials> {
          return ::faio::net::detail::socket_peer_credentials<PeerCredentials>(fd());
        });
  }
};

/** @brief Unix 监听器。pathname 的删除责任属于调用者，避免意外删除已复用路径。
 */
class UnixListener : public ::faio::net::detail::BaseListener<UnixListener, UnixStream, UnixAddr> {
 public:
  explicit UnixListener(Socket&& socket) : BaseListener{std::move(socket)} {}
};

/** @brief Unix 消息型 socket，支持无名 socketpair 与消息/辅助数据接口。 */
class UnixDatagram : public ::faio::net::detail::BaseDatagram<UnixDatagram, UnixAddr>,
                     public ::faio::net::detail::ImplSocketOptions<UnixDatagram> {
 public:
  explicit UnixDatagram(Socket&& socket) : BaseDatagram{std::move(socket)} {}

  [[nodiscard]] static auto unbound(io::io_context context = io::io_context::current())
      -> expected<UnixDatagram> {
    return Socket::create<UnixDatagram>(AF_UNIX, SOCK_DGRAM, 0, std::move(context));
  }

  [[nodiscard]] static auto pair(io::io_context context = io::io_context::current())
      -> expected<std::pair<UnixDatagram, UnixDatagram>> {
    int descriptors[2];
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors) < 0)
      return std::unexpected{make_error(errno)};
    owned_native_socket first{descriptors[0]}, second{descriptors[1]};
    if (auto prepared = Socket::prepare(first.get()); !prepared)
      return std::unexpected{prepared.error()};
    if (auto prepared = Socket::prepare(second.get()); !prepared)
      return std::unexpected{prepared.error()};
    auto a = Socket::adopt_checked(first.get(), context);
    if (!a)
      return std::unexpected{a.error()};
    (void)first.release();
    auto b = Socket::adopt_checked(second.get(), context);
    if (!b)
      return std::unexpected{b.error()};
    (void)second.release();
    return std::pair{UnixDatagram{std::move(*a)}, UnixDatagram{std::move(*b)}};
  }
};

/** @brief 配置后转换成 Unix stream/listener/datagram 的独占 socket。 */
class UnixSocket : public ::faio::net::detail::ImplSocketOptions<UnixSocket>,
                   public ::faio::net::detail::ImplLocalAddr<UnixSocket, UnixAddr> {
 public:
  explicit UnixSocket(Socket&& socket, bool datagram = false)
      : socket_{std::move(socket)}, datagram_{datagram} {}

  UnixSocket(UnixSocket&&) noexcept = default;

  UnixSocket& operator=(UnixSocket&&) noexcept = default;

  UnixSocket(const UnixSocket&) = delete;

  UnixSocket& operator=(const UnixSocket&) = delete;

  [[nodiscard]] static auto stream(io::io_context context = io::io_context::current())
      -> expected<UnixSocket> {
    auto socket = Socket::create(AF_UNIX, SOCK_STREAM, 0, std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    return UnixSocket{std::move(*socket)};
  }

  [[nodiscard]] static auto datagram(io::io_context context = io::io_context::current())
      -> expected<UnixSocket> {
    auto socket = Socket::create(AF_UNIX, SOCK_DGRAM, 0, std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    return UnixSocket{std::move(*socket), true};
  }

  /// @brief 读取原生句柄值，不在此接口转移关闭责任。
  [[nodiscard]] auto fd() const noexcept -> int { return socket_.fd(); }

  /// @brief 取得底层共享资源控制块，以保持句柄身份与所属域。
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }

  /// @brief 读取原生句柄值，不在此接口转移关闭责任。
  [[nodiscard]] auto as_native_handle() const noexcept -> int { return fd(); }

  [[nodiscard]] auto bind(const UnixAddr& address) -> expected<void> {
    return socket_.bind(address);
  }

  [[nodiscard]] auto listen(int backlog = SOMAXCONN) && -> expected<UnixListener> {
    if (datagram_)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    if (auto result = socket_.listen(backlog); !result)
      return std::unexpected{result.error()};
    return UnixListener{std::move(socket_)};
  }

  auto connect(UnixAddr address) && -> task<expected<UnixStream>> {
    return connect_owned(std::move(socket_), std::move(address), datagram_);
  }

  [[nodiscard]] auto into_datagram() && -> expected<UnixDatagram> {
    if (!datagram_)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    return UnixDatagram{std::move(socket_)};
  }

  /// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
  auto close() noexcept { return socket_.close(); }

  /// @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }

  /** @brief 接管 Unix stream/datagram fd，验证地址族和socket类型以后注册资源。
   */
  [[nodiscard]] static auto from_native(io::io_context context, owned_native_socket native)
      -> expected<UnixSocket> {
    int type{};
    socklen_t length = sizeof(type);
    if (::getsockopt(native.get(), SOL_SOCKET, SO_TYPE, &type, &length) < 0)
      return std::unexpected{make_error(errno)};
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    sockaddr_storage address{};
    length = sizeof(address);
    if (::getsockname(native.get(), reinterpret_cast<sockaddr*>(&address), &length) < 0)
      return std::unexpected{make_error(errno)};
    if (address.ss_family != AF_UNIX)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    if (auto result = Socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = Socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return UnixSocket{std::move(*socket), type == SOCK_DGRAM};
  }

 private:
  static auto connect_owned(Socket socket, UnixAddr address, bool datagram)
      -> task<expected<UnixStream>> {
    if (datagram)
      co_return std::unexpected{make_error(Error::InvalidSocketType)};
    auto result = co_await io::connect(socket.resource(), address.sockaddr(), address.length());
    if (!result)
      co_return std::unexpected{result.error()};
    co_return UnixStream{std::move(socket)};
  }

  Socket socket_;  ///< 拥有或共享底层 socket 包装，跨挂起保持原生资源身份。
  bool datagram_{};  ///< 是否采用数据报语义，零长度请求不能按流式无操作处理。
};
}

#endif
