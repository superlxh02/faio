/**
 * @file faio/detail/net/unix_domain/socket.hpp
 * @brief 套接字资源封装。
 */
#ifndef FAIO_DETAIL_NET_UNIX_DOMAIN_SOCKET_HPP
#define FAIO_DETAIL_NET_UNIX_DOMAIN_SOCKET_HPP
#include "faio/detail/net/socket_options.hpp"
#include "faio/detail/io/awaiter/connect.hpp"
#include "faio/detail/net/base/listener.hpp"
#include "faio/detail/net/base/stream.hpp"
#include "faio/detail/net/base/halves.hpp"
#include "faio/detail/net/base/datagram.hpp"
#include "faio/detail/net/unix_domain/address.hpp"
#include <tuple>

namespace faio::net::unix::detail {
using socket = ::faio::net::detail::socket;
using owned_native_socket = ::faio::net::detail::owned_native_socket;

/**  @brief 对端身份，macOS 的 pid 不由 getpeereid 提供，因此为 nullopt。 */
struct peer_credentials {
  std::uint32_t uid{};             /**< Unix 对端用户身份编号。 */
  std::uint32_t gid{};             /**< Unix 对端用户组身份编号。 */
  std::optional<std::int64_t> pid; /**< 平台可提供时记录的 Unix 对端进程编号。 */
};

/** @brief Unix 域字节流，提供异步读写及读写半部拆分。 */
class unix_stream : public ::faio::net::detail::base_stream<unix_stream, unix_addr>,
                    public ::faio::net::detail::impl_socket_options<unix_stream> {
 public:
  using read_half = ::faio::net::detail::basic_read_half<
      unix_stream,
      unix_addr,
      false>; /**< 借用式读半部类型，底层流须在整个借用期内存活。 */
  using write_half = ::faio::net::detail::basic_write_half<
      unix_stream,
      unix_addr,
      false>; /**< 借用式写半部类型，底层流须在整个借用期内存活。 */
  using owned_read_half = ::faio::net::detail::
      basic_read_half<unix_stream, unix_addr, true>; /**< 持有资源租约的拥有式读半部类型。 */
  using owned_write_half = ::faio::net::detail::
      basic_write_half<unix_stream, unix_addr, true>; /**< 持有资源租约的拥有式写半部类型。 */

  /** @brief 接管源对象的状态和资源租约。 */
  explicit unix_stream(socket&& socket) : base_stream{std::move(socket)} {}

  /** @brief 创建共享同一底层连接的读半部和写半部。 */
  [[nodiscard]] auto split() & -> std::pair<read_half, write_half> {
    auto identity = std::make_shared<char>();
    return {std::piecewise_construct,
            std::forward_as_tuple(share_socket(), identity),
            std::forward_as_tuple(share_socket(), identity)};
  }

  /** @brief 转移当前流的所有权，创建可独立移动的拥有型读写半部。 */
  [[nodiscard]] auto into_split() && -> std::pair<owned_read_half, owned_write_half> {
    auto identity = std::make_shared<char>();
    socket first = take_socket();
    socket second = first.share();
    return {std::piecewise_construct,
            std::forward_as_tuple(std::move(first), identity),
            std::forward_as_tuple(std::move(second), identity)};
  }

  /**  @brief 创建非阻塞全双工 socketpair，失败时两个原生 fd 都由 RAII 归还。 */
  [[nodiscard]] static auto pair(io::io_context context = io::io_context::current())
      -> expected<std::pair<unix_stream, unix_stream>> {
    int descriptors[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) < 0)
      return std::unexpected{make_error(errno)};
    owned_native_socket first{descriptors[0]}, second{descriptors[1]};
    if (auto prepared = socket::prepare(first.get()); !prepared)
      return std::unexpected{prepared.error()};
    if (auto prepared = socket::prepare(second.get()); !prepared)
      return std::unexpected{prepared.error()};
    auto a = socket::adopt_checked(first.get(), context);
    if (!a)
      return std::unexpected{a.error()};
    (void)first.release();
    auto b = socket::adopt_checked(second.get(), context);
    if (!b)
      return std::unexpected{b.error()};
    (void)second.release();
    return std::pair{unix_stream{std::move(*a)}, unix_stream{std::move(*b)}};
  }

  /**  @brief 查询已连接 Unix socket 对端的内核身份信息。 */
  [[nodiscard]] auto peer_credentials() const noexcept
      -> expected<::faio::net::unix::detail::peer_credentials> {
    return io::detail::with_resource(
        resource(),
        io::interest::NONE,
        [&]() -> expected<::faio::net::unix::detail::peer_credentials> {
          return ::faio::net::detail::socket_peer_credentials<
              ::faio::net::unix::detail::peer_credentials>(fd());
        });
  }
};

/**  @brief Unix 监听器。pathname 的删除责任属于调用者，避免意外删除已复用路径。 */
class unix_listener
    : public ::faio::net::detail::base_listener<unix_listener, unix_stream, unix_addr> {
 public:
  /** @brief 接管源对象的状态和资源租约。 */
  explicit unix_listener(socket&& socket) : base_listener{std::move(socket)} {}
};

/**  @brief Unix 消息型 socket，支持无名 socketpair，以及消息与辅助数据接口。 */
class unix_datagram : public ::faio::net::detail::base_datagram<unix_datagram, unix_addr>,
                      public ::faio::net::detail::impl_socket_options<unix_datagram> {
 public:
  /** @brief 接管源对象的状态和资源租约。 */
  explicit unix_datagram(socket&& socket) : base_datagram{std::move(socket)} {}

  /** @brief 创建未绑定的数据报 socket 并关联指定 IO 上下文。 */
  [[nodiscard]] static auto unbound(io::io_context context = io::io_context::current())
      -> expected<unix_datagram> {
    return socket::create<unix_datagram>(AF_UNIX, SOCK_DGRAM, 0, std::move(context));
  }

  /** @brief 创建一对已互联的 Unix 数据报 socket。 */
  [[nodiscard]] static auto pair(io::io_context context = io::io_context::current())
      -> expected<std::pair<unix_datagram, unix_datagram>> {
    int descriptors[2];
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors) < 0)
      return std::unexpected{make_error(errno)};
    owned_native_socket first{descriptors[0]}, second{descriptors[1]};
    if (auto prepared = socket::prepare(first.get()); !prepared)
      return std::unexpected{prepared.error()};
    if (auto prepared = socket::prepare(second.get()); !prepared)
      return std::unexpected{prepared.error()};
    auto a = socket::adopt_checked(first.get(), context);
    if (!a)
      return std::unexpected{a.error()};
    (void)first.release();
    auto b = socket::adopt_checked(second.get(), context);
    if (!b)
      return std::unexpected{b.error()};
    (void)second.release();
    return std::pair{unix_datagram{std::move(*a)}, unix_datagram{std::move(*b)}};
  }
};

/**  @brief 可先配置，再转换为 Unix stream/listener/datagram 的独占 socket。 */
class unix_socket : public ::faio::net::detail::impl_socket_options<unix_socket>,
                    public ::faio::net::detail::impl_local_addr<unix_socket, unix_addr> {
 public:
  /** @brief 接管源对象的状态和资源租约。 */
  explicit unix_socket(socket&& socket, bool datagram = false)
      : socket_{std::move(socket)}, datagram_{datagram} {}

  /** @brief 接管源对象的状态和资源租约。 */
  unix_socket(unix_socket&&) noexcept = default;

  /** @brief 移动赋值资源及状态。 */
  unix_socket& operator=(unix_socket&&) noexcept = default;

  /** @brief 禁止复制此对象。 */
  unix_socket(const unix_socket&) = delete;

  /** @brief 禁止复制赋值。 */
  unix_socket& operator=(const unix_socket&) = delete;

  /** @brief 创建未连接的 Unix 字节流 socket。 */
  [[nodiscard]] static auto stream(io::io_context context = io::io_context::current())
      -> expected<unix_socket> {
    auto socket = socket::create(AF_UNIX, SOCK_STREAM, 0, std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    return unix_socket{std::move(*socket)};
  }

  /** @brief 创建未绑定的 Unix 数据报 socket。 */
  [[nodiscard]] static auto datagram(io::io_context context = io::io_context::current())
      -> expected<unix_socket> {
    auto socket = socket::create(AF_UNIX, SOCK_DGRAM, 0, std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    return unix_socket{std::move(*socket), true};
  }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto fd() const noexcept -> int { return socket_.fd(); }

  /** @brief 取得底层共享资源控制块，保持句柄身份及所属域有效。 */
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto as_native_handle() const noexcept -> int { return fd(); }

  /** @brief 将 socket 绑定到给定本地地址。 */
  [[nodiscard]] auto bind(const unix_addr& address) -> expected<void> {
    return socket_.bind(address);
  }

  /** @brief 进入监听状态并设置待接纳连接队列上限。 */
  [[nodiscard]] auto listen(int backlog = SOMAXCONN) && -> expected<unix_listener> {
    if (datagram_)
      return std::unexpected{make_error(::faio::error::INVALID_SOCKET_TYPE)};
    if (auto result = socket_.listen(backlog); !result)
      return std::unexpected{result.error()};
    return unix_listener{std::move(socket_)};
  }

  /** @brief 连接到指定对端，返回连接结果。 */
  auto connect(unix_addr address) && -> task<expected<unix_stream>> {
    return connect_owned(std::move(socket_), std::move(address), datagram_);
  }

  /** @brief 转移 socket 的所有权并转换为数据报接口。 */
  [[nodiscard]] auto into_datagram() && -> expected<unix_datagram> {
    if (!datagram_)
      return std::unexpected{make_error(::faio::error::INVALID_SOCKET_TYPE)};
    return unix_datagram{std::move(socket_)};
  }

  /** @brief 异步关闭共享套接字，等待在途操作安全结束。 */
  auto close() noexcept { return socket_.close(); }

  /** @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。 */
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }

  /**  @brief 接管 Unix stream/datagram fd，验证地址族和 socket 类型以后注册资源。 */
  [[nodiscard]] static auto from_native(io::io_context context, owned_native_socket native)
      -> expected<unix_socket> {
    int type{};
    socklen_t length = sizeof(type);
    if (::getsockopt(native.get(), SOL_SOCKET, SO_TYPE, &type, &length) < 0)
      return std::unexpected{make_error(errno)};
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
      return std::unexpected{make_error(::faio::error::INVALID_SOCKET_TYPE)};
    sockaddr_storage address{};
    length = sizeof(address);
    if (::getsockname(native.get(), reinterpret_cast<sockaddr*>(&address), &length) < 0)
      return std::unexpected{make_error(errno)};
    if (address.ss_family != AF_UNIX)
      return std::unexpected{make_error(::faio::error::INVALID_SOCKET_TYPE)};
    if (auto result = socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return unix_socket{std::move(*socket), type == SOCK_DGRAM};
  }

 private:
  /** @brief 持有 socket 直至连接完成，再交付连接流或错误。 */
  static auto connect_owned(socket socket, unix_addr address, bool datagram)
      -> task<expected<unix_stream>> {
    if (datagram)
      co_return std::unexpected{make_error(::faio::error::INVALID_SOCKET_TYPE)};
    auto result = co_await io::connect(socket.resource(), address.sockaddr(), address.length());
    if (!result)
      co_return std::unexpected{result.error()};
    co_return unix_stream{std::move(socket)};
  }

  socket socket_;   /**< 持有或共享底层 socket 包装，在挂起期间保持原生资源身份有效。 */
  bool datagram_{}; /**< 是否采用数据报语义，零长度请求不能按流式无操作处理。 */
};
}  // namespace faio::net::unix::detail

#endif
