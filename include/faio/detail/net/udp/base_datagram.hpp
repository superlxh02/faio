#ifndef FAIO_DETAIL_NET_UDP_BASE_DATAGRAM_HPP
#define FAIO_DETAIL_NET_UDP_BASE_DATAGRAM_HPP
#include "faio/detail/net/common/addr_util.hpp"
#include "faio/detail/net/common/datagram_recv.hpp"
#include "faio/detail/net/common/datagram_send.hpp"
#include "faio/detail/net/common/socket.hpp"
#include <functional>
#include <span>
#include <vector>
namespace faio::net::detail {
/** @brief 消息型 socket 的共同实现；保持 UDP/Unix 数据报边界，不提供字节流
 * write_all。
 * @details 同一方向的在途请求由资源 gate 仲裁；多个接收者应使用单一接收循环
 * 与有界 channel 分派。ready 只是观察，不消费数据报，允许假就绪。
 */
template <class Datagram, class Addr>
class BaseDatagram : public ImplSend<BaseDatagram<Datagram, Addr>, Addr>,
                     public ImplRecv<BaseDatagram<Datagram, Addr>, Addr>,
                     public ImplLocalAddr<BaseDatagram<Datagram, Addr>, Addr>,
                     public ImplPeerAddr<BaseDatagram<Datagram, Addr>, Addr> {
protected:
  explicit BaseDatagram(Socket &&socket) : socket_{std::move(socket)} {}

public:
  BaseDatagram(BaseDatagram &&) noexcept = default;
  auto operator=(BaseDatagram &&) noexcept -> BaseDatagram & = default;
  BaseDatagram(const BaseDatagram &) = delete;
  auto operator=(const BaseDatagram &) -> BaseDatagram & = delete;
  ~BaseDatagram() = default;
  /** @brief 按值保存目标地址；连接数据报只设置默认目标及入包过滤，不执行 TCP
   * 握手。 */
  auto connect(Addr address) const -> task<expected<void>> {
    return connect_owned(resource(), std::move(address));
  }
  auto shutdown(io::ShutdownBehavior direction =
                    io::ShutdownBehavior::Both) const noexcept {
    return io::shutdown(resource(), static_cast<int>(direction));
  }
  auto close() noexcept { return socket_.close(); }
  [[nodiscard]] auto fd() const noexcept -> int { return socket_.fd(); }
  [[nodiscard]] auto as_native_handle() const noexcept -> int { return fd(); }
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }
  /** @brief 克隆安全拥有者，所有副本使用同一资源，不 dup 原生 fd。 */
  [[nodiscard]] auto share() const -> Datagram {
    return Datagram{socket_.share()};
  }
  /** @brief 独立观察消息 socket 就绪，不消费数据报；返回方向独立的 Ready 快照。
   */
  auto ready(io::Interest interest) const noexcept {
    return io::ready(resource(), interest);
  }
  auto readable() const noexcept { return ready(io::Interest::readable); }
  auto writable() const noexcept { return ready(io::Interest::writable); }
  template <class F> auto try_io(io::Interest interest, F &&function) const {
    return io::detail::with_resource(resource(), interest, [&] {
      if constexpr (std::invocable<F &, int>)
        return std::invoke(function, fd());
      else
        return std::invoke(function);
    });
  }
  /** @brief 自定义非阻塞系统调用；would_block 后挂起就绪观察，再重试。 */
  template <class F>
  auto async_io(io::Interest interest, F function) const
      -> task<decltype(try_io(interest, function))> {
    return async_io_impl<decltype(try_io(interest, function))>(
        resource(), interest, std::move(function));
  }
  /** @brief 只在资源没有在途操作时导出，关闭责任由返回的 RAII owner 接管。 */
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }
  [[nodiscard]] static auto from_native(io::io_context context,
                                        owned_native_socket native)
      -> expected<Datagram> {
    int type{};
    socklen_t length = sizeof(type);
    if (::getsockopt(native.get(), SOL_SOCKET, SO_TYPE, &type, &length) < 0)
      return std::unexpected{make_error(errno)};
    if (type != SOCK_DGRAM)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    if (auto valid = Socket::validate_family<Addr>(native.get()); !valid)
      return std::unexpected{valid.error()};
    if (auto result = Socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = Socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return Datagram{std::move(*socket)};
  }
  [[nodiscard]] static auto
  bind(const Addr &address, io::io_context context = io::io_context::current())
      -> expected<Datagram> {
    auto socket =
        Socket::create(address.family(), SOCK_DGRAM, 0, std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    if (auto result = socket->bind(address); !result)
      return std::unexpected{result.error()};
    return Datagram{std::move(*socket)};
  }
  [[nodiscard]] static auto bind(io::io_context context, const Addr &address)
      -> expected<Datagram> {
    return bind(address, std::move(context));
  }
  [[nodiscard]] static auto
  bind(std::span<const Addr> addresses,
       io::io_context context = io::io_context::current())
      -> expected<Datagram> {
    Error last{Error::InvalidAddresses};
    for (const auto &address : addresses) {
      auto result = bind(address, context);
      if (result)
        return result;
      last = result.error();
    }
    return std::unexpected{last};
  }
  [[nodiscard]] static auto bind(io::io_context context,
                                 std::span<const Addr> addresses)
      -> expected<Datagram> {
    return bind(addresses, std::move(context));
  }

private:
  /** @brief resource 与 callable 拥有型快照允许 task 在包装对象移动之后启动。
   */
  template <class Result, class F>
  static auto
  async_io_impl(std::shared_ptr<io::detail::resource_state> resource,
                io::Interest interest, F function) -> task<Result> {
    while (true) {
      auto result = io::detail::with_resource(resource, interest, [&] {
        if constexpr (std::invocable<F &, int>)
          return std::invoke(function, resource->fd());
        else
          return std::invoke(function);
      });
      if (result || (result.error().value() != EAGAIN &&
                     result.error().value() != EWOULDBLOCK))
        co_return result;
      auto readiness = co_await io::ready(resource, interest);
      if (!readiness)
        co_return std::unexpected{readiness.error()};
    }
  }
  static auto
  connect_owned(std::shared_ptr<io::detail::resource_state> resource,
                Addr address) -> task<expected<void>> {
    co_return co_await io::connect(std::move(resource), address.sockaddr(),
                                   address.length());
  }
  Socket socket_;
};
} // namespace faio::net::detail
#endif
