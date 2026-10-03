#ifndef FAIO_DETAIL_NET_TCP_BASE_STREAM_HPP
#define FAIO_DETAIL_NET_TCP_BASE_STREAM_HPP
#include "faio/detail/net/common/platform.hpp"
#include "faio/detail/net/common/addr_util.hpp"
#include "faio/detail/net/common/socket.hpp"
#include "faio/detail/net/common/sockopt.hpp"
#include "faio/detail/net/common/stream_read.hpp"
#include "faio/detail/net/common/stream_write.hpp"
#include <functional>
#include <optional>
#include <type_traits>

namespace faio::net::detail {
/** @brief 字节流的后端无关接口；每条连接保持创建时 IO domain 的共享 lease。 */
template <class Stream, class Addr>
class BaseStream : public ImplStreamRead<BaseStream<Stream, Addr>>,
                   public ImplStreamWrite<BaseStream<Stream, Addr>>,
                   public ImplLocalAddr<BaseStream<Stream, Addr>, Addr>,
                   public ImplPeerAddr<BaseStream<Stream, Addr>, Addr> {
 protected:
  explicit BaseStream(Socket&& socket) : socket_{std::move(socket)} {}

  [[nodiscard]] auto share_socket() const -> Socket { return socket_.share(); }

  [[nodiscard]] auto take_socket() -> Socket { return std::move(socket_); }

 public:
  using address_type = Addr;  ///< 字节流地址协议，用于后端中立的原生能力选择。
  BaseStream(BaseStream&&) noexcept = default;

  auto operator=(BaseStream&&) noexcept -> BaseStream& = default;

  BaseStream(const BaseStream&) = delete;

  auto operator=(const BaseStream&) -> BaseStream& = delete;

  ~BaseStream() = default;

  auto shutdown(io::ShutdownBehavior direction = io::ShutdownBehavior::Write) noexcept {
    return socket_.shutdown(direction);
  }

  auto close() noexcept { return socket_.close(); }

  [[nodiscard]] auto fd() const noexcept -> native_socket_type { return socket_.fd(); }

  [[nodiscard]] auto as_native_handle() const noexcept -> native_socket_type { return fd(); }

  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }

  [[nodiscard]] auto context() const noexcept { return socket_.context(); }

  /** @brief 独立观察指定方向；不消费字节，不占用实际读写方向执行权。
   * @details 返回 expected<io::Ready>；读、写关闭提示分别查询，允许假就绪，
   *          try_* 返回 EAGAIN 后清除对应旧就绪位并重新等待。
   */
  auto ready(io::Interest interest) const noexcept { return io::ready(resource(), interest); }

  auto readable() const noexcept { return ready(io::Interest::readable); }

  auto writable() const noexcept { return ready(io::Interest::writable); }

  /** @brief 自定义短非阻塞 syscall，所选读写方向由资源 gate 保证互斥。
   * @details callable 返回 expected<T>，接收 fd
   * 或无参数；不得递归进入同一对象。 本显式同步接口在 io_uring 上仍执行
   * callable，不将其转换为原生 opcode。
   */
  template <class F>
  auto try_io(io::Interest interest, F&& function) const {
    return io::detail::with_resource(resource(), interest, [&] {
      if constexpr (std::invocable<F&, native_socket_type>)
        return std::invoke(function, fd());
      else
        return std::invoke(function);
    });
  }

  /** @brief 在 would_block 时观察 readiness 并再次执行自定义非阻塞 syscall。
   * @details callable 与资源按值保存；io_uring 观察使用 POLL_ADD，
   *          内建 read/write 使用各自原生请求，不经过本兼容接口。
   */
  template <class F>
  auto async_io(io::Interest interest, F function) const
      -> task<decltype(try_io(interest, function))> {
    return async_io_impl<decltype(try_io(interest, function))>(
        resource(), interest, std::move(function));
  }

  /** @brief 导出只在无在途操作时成功；所有半边共享的状态同时变为 detached。 */
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }

  [[nodiscard]] static auto from_native(io::io_context context, owned_native_socket native)
      -> expected<Stream> {
    if (auto placement = native.validate_import(context); !placement)
      return std::unexpected{placement.error()};
    int type{};
    socklen_t length = sizeof(type);
    if (socket_getsockopt(native.get(), SOL_SOCKET, SO_TYPE, &type, &length) < 0)
      return std::unexpected{socket_error()};
    if (type != SOCK_STREAM)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    if (auto valid = Socket::validate_family<Addr>(native.get()); !valid)
      return std::unexpected{valid.error()};
    if (auto result = Socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = Socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    Socket::release_imported(native);
    return Stream{std::move(*socket)};
  }

  /** @brief 建立数字端点连接；地址按值保存在组合协程帧，避免悬空借用。 */
  static auto connect(Addr address, io::io_context context = io::io_context::current())
      -> task<expected<Stream>> {
    // 异步连接组合的句柄创建同样使用统一原生 SOCKET 请求，不预先 ::socket。
    auto socket =
        co_await Socket::create_async(address.family(), SOCK_STREAM, 0, std::move(context));
    if (!socket)
      co_return std::unexpected{socket.error()};
    auto result = co_await io::connect(socket->resource(), address.sockaddr(), address.length());
    if (!result)
      co_return std::unexpected{result.error()};
    co_return Stream{std::move(*socket)};
  }

  static auto connect(io::io_context context, Addr address) -> task<expected<Stream>> {
    co_return co_await connect(std::move(address), std::move(context));
  }

  /** @brief 按输入顺序尝试多个端点；每个失败 socket 在下一次尝试前释放。 */
  static auto connect(std::span<const Addr> addresses,
                      io::io_context context = io::io_context::current())
      -> task<expected<Stream>> {
    Error last{Error::InvalidAddresses};
    for (const auto& address : addresses) {
      auto stream = co_await connect(address, context);
      if (stream)
        co_return stream;
      last = stream.error();
    }
    co_return std::unexpected{last};
  }

  static auto connect(io::io_context context, std::span<const Addr> addresses)
      -> task<expected<Stream>> {
    co_return co_await connect(addresses, std::move(context));
  }

 private:
  /** @brief resource 与 callable 按值保存，task 延迟恢复不依赖原包装对象的
   * this。 */
  template <class Result, class F>
  static auto async_io_impl(std::shared_ptr<io::detail::resource_state> resource,
                            io::Interest interest,
                            F function) -> task<Result> {
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

  Socket socket_;
};
}  // namespace faio::net::detail
#endif
