/**
 * @file faio/detail/net/base/datagram.hpp
 * @brief 数据报公共封装。
 */
#ifndef FAIO_DETAIL_NET_BASE_DATAGRAM_HPP
#define FAIO_DETAIL_NET_BASE_DATAGRAM_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/io/awaiter/connect.hpp"
#include "faio/detail/io/awaiter/ready.hpp"
#include "faio/detail/io/awaiter/shutdown.hpp"
#include <concepts>
#include "faio/detail/net/base/address.hpp"
#include "faio/detail/net/base/datagram_recv.hpp"
#include "faio/detail/net/base/datagram_send.hpp"
#include "faio/detail/net/base/socket.hpp"
#include <functional>
#include <span>

namespace faio::net::detail {
/**
 *  @brief 消息型 socket 的共同实现；保持 UDP/Unix 数据报边界，不提供字节流
 * write_all。
 * @details 同一方向的在途请求由资源 gate 仲裁；多个接收者应使用单一接收循环
 * 与有界 channel 分派。ready 只是观察，不消费数据报，允许假就绪。
 */
template <class Datagram, class Addr>
class base_datagram : public impl_send<base_datagram<Datagram, Addr>, Addr>,
                      public impl_recv<base_datagram<Datagram, Addr>, Addr>,
                      public impl_local_addr<base_datagram<Datagram, Addr>, Addr>,
                      public impl_peer_addr<base_datagram<Datagram, Addr>, Addr> {
 protected:
  /** @brief 接管源对象的状态和资源租约。 */
  explicit base_datagram(socket&& socket) : socket_{std::move(socket)} {}

 public:
  /** @brief 接管源对象的状态和资源租约。 */
  base_datagram(base_datagram&&) noexcept = default;

  /** @brief 移动赋值资源及状态。 */
  auto operator=(base_datagram&&) noexcept -> base_datagram& = default;

  /** @brief 禁止复制此对象。 */
  base_datagram(const base_datagram&) = delete;

  /** @brief 禁止复制赋值。 */
  auto operator=(const base_datagram&) -> base_datagram& = delete;

  /** @brief 释放数据报 socket 的资源租约。 */
  ~base_datagram() = default;

  /**
   *  @brief 按值保存目标地址；连接数据报只设置默认目标及入包过滤，不执行 TCP
   * 握手。
   */
  auto connect(Addr address) const -> task<expected<void>> {
    return connect_owned(resource(), std::move(address));
  }

  /** @brief 请求关闭 socket 的指定传输方向。 */
  auto shutdown(io::shutdown_behavior direction = io::shutdown_behavior::BOTH) const noexcept {
    return io::shutdown(resource(), static_cast<int>(direction));
  }

  /** @brief 异步关闭共享套接字，等待在途操作安全结束。 */
  auto close() noexcept { return socket_.close(); }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto fd() const noexcept -> native_socket_type { return socket_.fd(); }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto as_native_handle() const noexcept -> native_socket_type { return fd(); }

  /** @brief 取得底层共享资源控制块，保持句柄身份及所属域有效。 */
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }

  /** @brief 取得该对象关联的 I/O 上下文租约。 */
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }

  /**  @brief 克隆安全拥有者，所有副本使用同一资源，不 dup 原生 fd。 */
  [[nodiscard]] auto share() const -> Datagram { return Datagram{socket_.share()}; }

  /**  @brief 独立观察消息 socket 就绪，不消费数据报；返回方向独立的 Ready 快照。 */
  auto ready(io::interest interest) const noexcept { return io::ready(resource(), interest); }

  /** @brief 等待 socket 或管道可读，不消费数据。 */
  auto readable() const noexcept { return ready(io::interest::READABLE); }

  /** @brief 等待 socket 或管道可写，不执行实际写入。 */
  auto writable() const noexcept { return ready(io::interest::WRITABLE); }

  /** @brief 在底层资源方向校验后执行一次即时 IO 回调。 */
  template <class F>
  auto try_io(io::interest interest, F&& function) const {
    return io::detail::with_resource(resource(), interest, [&] {
      if constexpr (std::invocable<F&, native_socket_type>)
        return std::invoke(function, fd());
      else
        return std::invoke(function);
    });
  }

  /**  @brief 自定义非阻塞系统调用；would_block 后挂起就绪观察，再重试。 */
  template <class F>
  auto async_io(io::interest interest, F function) const
      -> task<decltype(try_io(interest, function))> {
    return async_io_impl<decltype(try_io(interest, function))>(
        resource(), interest, std::move(function));
  }

  /**  @brief 只在资源没有在途操作时导出，关闭责任由返回的 RAII owner 接管。 */
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }

  /** @brief 接管显式传入的原生句柄，按平台规则建立资源包装。 */
  [[nodiscard]] static auto from_native(io::io_context context, owned_native_socket native)
      -> expected<Datagram> {
    if (auto placement = native.validate_import(context); !placement)
      return std::unexpected{placement.error()};
    int type{};
    socklen_t length = sizeof(type);
    if (socket_getsockopt(native.get(), SOL_SOCKET, SO_TYPE, &type, &length) < 0)
      return std::unexpected{socket_error()};
    if (type != SOCK_DGRAM)
      return std::unexpected{make_error(::faio::error::INVALID_SOCKET_TYPE)};
    if (auto valid = socket::validate_family<Addr>(native.get()); !valid)
      return std::unexpected{valid.error()};
    if (auto result = socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    socket::release_imported(native);
    return Datagram{std::move(*socket)};
  }

  /** @brief 将 socket 绑定到给定本地地址。 */
  [[nodiscard]] static auto bind(const Addr& address,
                                 io::io_context context = io::io_context::current())
      -> expected<Datagram> {
    auto socket = socket::create(address.family(), SOCK_DGRAM, 0, std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    if (auto result = socket->bind(address); !result)
      return std::unexpected{result.error()};
    return Datagram{std::move(*socket)};
  }

  /** @brief 将 socket 绑定到给定本地地址。 */
  [[nodiscard]] static auto bind(io::io_context context, const Addr& address)
      -> expected<Datagram> {
    return bind(address, std::move(context));
  }

  /** @brief 将 socket 绑定到给定本地地址。 */
  [[nodiscard]] static auto bind(std::span<const Addr> addresses,
                                 io::io_context context = io::io_context::current())
      -> expected<Datagram> {
    error last{::faio::error::INVALID_ADDRESSES};
    for (const auto& address : addresses) {
      auto result = bind(address, context);
      if (result)
        return result;
      last = result.error();
    }
    return std::unexpected{last};
  }

  /** @brief 将 socket 绑定到给定本地地址。 */
  [[nodiscard]] static auto bind(io::io_context context, std::span<const Addr> addresses)
      -> expected<Datagram> {
    return bind(addresses, std::move(context));
  }

 private:
  /**  @brief resource 与 callable 的拥有型快照使 task 可在包装对象移动后启动。 */
  template <class result_type, class F>
  static auto async_io_impl(std::shared_ptr<io::detail::resource_state> resource,
                            io::interest interest,
                            F function) -> task<result_type> {
    while (true) {
      auto result = io::detail::with_resource(resource, interest, [&] {
        if constexpr (std::invocable<F&, native_socket_type>)
          return std::invoke(function, resource->fd());
        else
          return std::invoke(function);
      });
      if (result || !socket_would_block(result.error()))
        co_return result;
      auto readiness = co_await io::ready(resource, interest);
      if (!readiness)
        co_return std::unexpected{readiness.error()};
    }
  }

  /** @brief 持有数据报套接字直至连接完成，再交付结果或错误。 */
  static auto connect_owned(std::shared_ptr<io::detail::resource_state> resource, Addr address)
      -> task<expected<void>> {
    co_return co_await io::connect(std::move(resource), address.sockaddr(), address.length());
  }

  socket socket_; /**< 持有或共享底层 socket 包装，在挂起期间保持原生资源身份有效。 */
};
}  // namespace faio::net::detail
#endif
