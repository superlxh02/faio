/**
 * @file faio/detail/net/base/stream.hpp
 * @brief 字节流公共封装。
 */
#ifndef FAIO_DETAIL_NET_BASE_STREAM_HPP
#define FAIO_DETAIL_NET_BASE_STREAM_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/io/awaiter/connect.hpp"
#include "faio/detail/io/awaiter/ready.hpp"
#include <concepts>
#include "faio/detail/net/base/address.hpp"
#include "faio/detail/net/base/socket.hpp"
#include "faio/detail/net/base/stream_read.hpp"
#include "faio/detail/net/base/stream_write.hpp"
#include <functional>

namespace faio::net::detail {
/**  @brief 字节流的后端无关接口；每条连接持有创建时 IO domain 的共享 lease。 */
template <class Stream, class Addr>
class base_stream : public impl_stream_read<base_stream<Stream, Addr>>,
                    public impl_stream_write<base_stream<Stream, Addr>>,
                    public impl_local_addr<base_stream<Stream, Addr>, Addr>,
                    public impl_peer_addr<base_stream<Stream, Addr>, Addr> {
 protected:
  /** @brief 接管源对象的状态和资源租约。 */
  explicit base_stream(socket&& socket) : socket_{std::move(socket)} {}

  /** @brief 共享底层 socket 资源控制块。 */
  [[nodiscard]] auto share_socket() const -> socket { return socket_.share(); }

  /** @brief 转移当前流的 socket 包装。 */
  [[nodiscard]] auto take_socket() -> socket { return std::move(socket_); }

 public:
  using address_type = Addr; /**< 字节流地址协议，用于在不依赖后端的情况下选择原生能力。 */
  /** @brief 接管源对象的状态和资源租约。 */
  base_stream(base_stream&&) noexcept = default;

  /** @brief 移动赋值资源及状态。 */
  auto operator=(base_stream&&) noexcept -> base_stream& = default;

  /** @brief 禁止复制此对象。 */
  base_stream(const base_stream&) = delete;

  /** @brief 禁止复制赋值。 */
  auto operator=(const base_stream&) -> base_stream& = delete;

  /** @brief 释放字节流的 socket 资源租约。 */
  ~base_stream() = default;

  /** @brief 请求关闭 socket 的指定传输方向。 */
  auto shutdown(io::shutdown_behavior direction = io::shutdown_behavior::WRITE) noexcept {
    return socket_.shutdown(direction);
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

  /**
   *  @brief 独立观察指定方向；不消费字节，不占用实际读写方向执行权。
   * @details 返回 expected<io::Ready>；读、写关闭提示分别查询，允许假就绪，
   *          try_* 返回 EAGAIN 后清除对应旧就绪位并重新等待。
   */
  auto ready(io::interest interest) const noexcept { return io::ready(resource(), interest); }

  /** @brief 等待 socket 或管道可读，不消费数据。 */
  auto readable() const noexcept { return ready(io::interest::READABLE); }

  /** @brief 等待 socket 或管道可写，不执行实际写入。 */
  auto writable() const noexcept { return ready(io::interest::WRITABLE); }

  /**
   *  @brief 自定义短非阻塞 syscall，所选读写方向由资源 gate 保证互斥。
   * @details callable 返回 expected<T>，接收 fd
   * 或无参数；不得递归进入同一对象。 本显式同步接口在 io_uring 上仍调用 callable，不将其转换为原生 opcode。
   */
  template <class F>
  auto try_io(io::interest interest, F&& function) const {
    return io::detail::with_resource(resource(), interest, [&] {
      if constexpr (std::invocable<F&, native_socket_type>)
        return std::invoke(function, fd());
      else
        return std::invoke(function);
    });
  }

  /**
   *  @brief 在 would_block 时观察 readiness 并再次执行自定义非阻塞 syscall。
   * @details callable 与资源按值保存；io_uring 观察使用 POLL_ADD，
   *          内建 read/write 使用各自原生请求，不经过本兼容接口。
   */
  template <class F>
  auto async_io(io::interest interest, F function) const
      -> task<decltype(try_io(interest, function))> {
    return async_io_impl<decltype(try_io(interest, function))>(
        resource(), interest, std::move(function));
  }

  /**  @brief 导出只在无在途操作时成功；所有半部共享的状态同时变为 detached。 */
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }

  /** @brief 接管显式传入的原生句柄，按平台规则建立资源包装。 */
  [[nodiscard]] static auto from_native(io::io_context context, owned_native_socket native)
      -> expected<Stream> {
    if (auto placement = native.validate_import(context); !placement)
      return std::unexpected{placement.error()};
    int type{};
    socklen_t length = sizeof(type);
    if (socket_getsockopt(native.get(), SOL_SOCKET, SO_TYPE, &type, &length) < 0)
      return std::unexpected{socket_error()};
    if (type != SOCK_STREAM)
      return std::unexpected{make_error(::faio::error::INVALID_SOCKET_TYPE)};
    if (auto valid = socket::validate_family<Addr>(native.get()); !valid)
      return std::unexpected{valid.error()};
    if (auto result = socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    socket::release_imported(native);
    return Stream{std::move(*socket)};
  }

  /**  @brief 建立数字端点连接；地址按值保存在组合协程帧，避免悬空借用。 */
  static auto connect(Addr address, io::io_context context = io::io_context::current())
      -> task<expected<Stream>> {
    auto socket =
        co_await socket::create_async(address.family(), SOCK_STREAM, 0, std::move(context));
    if (!socket)
      co_return std::unexpected{socket.error()};
    auto result = co_await io::connect(socket->resource(), address.sockaddr(), address.length());
    if (!result)
      co_return std::unexpected{result.error()};
    co_return Stream{std::move(*socket)};
  }

  /** @brief 连接到指定对端，返回连接结果。 */
  static auto connect(io::io_context context, Addr address) -> task<expected<Stream>> {
    co_return co_await connect(std::move(address), std::move(context));
  }

  /**  @brief 按输入顺序尝试多个端点；每个失败 socket 在下一次尝试前释放。 */
  static auto connect(std::span<const Addr> addresses,
                      io::io_context context = io::io_context::current())
      -> task<expected<Stream>> {
    error last{::faio::error::INVALID_ADDRESSES};
    for (const auto& address : addresses) {
      auto stream = co_await connect(address, context);
      if (stream)
        co_return stream;
      last = stream.error();
    }
    co_return std::unexpected{last};
  }

  /** @brief 连接到指定对端，返回连接结果。 */
  static auto connect(io::io_context context, std::span<const Addr> addresses)
      -> task<expected<Stream>> {
    co_return co_await connect(addresses, std::move(context));
  }

 private:
  /**
   *  @brief resource 与 callable 按值保存，task 延迟恢复不依赖原包装对象的
   * this。
   */
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

  socket socket_; /**< 持有或共享底层 socket 包装，在挂起期间保持原生资源身份有效。 */
};
}  // namespace faio::net::detail
#endif
