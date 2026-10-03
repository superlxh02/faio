#ifndef FAIO_DETAIL_NET_UNIX_SOCKET_HPP
#define FAIO_DETAIL_NET_UNIX_SOCKET_HPP
#include "faio/detail/net/common/sockopt.hpp"
#include "faio/detail/net/tcp/base_listener.hpp"
#include "faio/detail/net/tcp/base_stream.hpp"
#include "faio/detail/net/tcp/halves.hpp"
#include "faio/detail/net/udp/base_datagram.hpp"
#include "faio/detail/net/unix/address.hpp"
#include <tuple>

namespace faio::net::unix::detail {
using Socket = ::faio::net::detail::Socket;
using owned_native_socket = ::faio::net::detail::owned_native_socket;

/** @brief 对端身份，macOS 的 pid 不由 getpeereid 提供，因此为 nullopt。 */
struct PeerCredentials {
  std::uint32_t uid{};
  std::uint32_t gid{};
  std::optional<std::int64_t> pid;
};

class UnixStream : public ::faio::net::detail::BaseStream<UnixStream, UnixAddr>,
                   public ::faio::net::detail::ImplSocketOptions<UnixStream> {
 public:
  using ReadHalf = ::faio::net::detail::BasicReadHalf<UnixStream, UnixAddr, false>;
  using WriteHalf = ::faio::net::detail::BasicWriteHalf<UnixStream, UnixAddr, false>;
  using OwnedReadHalf = ::faio::net::detail::BasicReadHalf<UnixStream, UnixAddr, true>;
  using OwnedWriteHalf = ::faio::net::detail::BasicWriteHalf<UnixStream, UnixAddr, true>;

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
#if defined(__linux__)
          struct ucred credentials{};
          socklen_t size = sizeof(credentials);
          if (::getsockopt(fd(), SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0)
            return std::unexpected{make_error(errno)};
          return PeerCredentials{credentials.uid, credentials.gid, credentials.pid};
#else
                    uid_t uid{};
                    gid_t gid{};
                    if (::getpeereid(fd(), &uid, &gid) < 0) return std::unexpected{make_error(errno)};
                    return PeerCredentials{uid, gid, std::nullopt};
#endif
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

  [[nodiscard]] auto fd() const noexcept -> int { return socket_.fd(); }

  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }

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

  auto close() noexcept { return socket_.close(); }

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

  Socket socket_;
  bool datagram_{};
};
}  // namespace faio::net::unix::detail

namespace faio::net::unix {
using address = detail::UnixAddr;
using UnixAddr = detail::UnixAddr;
using UnixSocket = detail::UnixSocket;
using UnixListener = detail::UnixListener;
using UnixStream = detail::UnixStream;
using UnixDatagram = detail::UnixDatagram;
using PeerCredentials = detail::PeerCredentials;
}  // namespace faio::net::unix
#endif
