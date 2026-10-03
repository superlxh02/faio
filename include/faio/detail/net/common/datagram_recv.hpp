#ifndef FAIO_DETAIL_NET_COMMON_DATAGRAM_RECV_HPP
#define FAIO_DETAIL_NET_COMMON_DATAGRAM_RECV_HPP
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/buffer.hpp"
#include "faio/detail/net/common/awaiter_options.hpp"
#include "faio/detail/net/common/socket.hpp"
#include <algorithm>
#include <cstddef>
#include <span>
#include <sys/uio.h>
#include <vector>
namespace faio::net::detail {
/** @brief 一条数据报的接收结果；零字节是合法包，truncated 指示 payload 截断。
 */
template <class Addr> struct DatagramMessage {
  std::size_t copied_bytes{};
  Addr peer{};
  bool truncated{};
  bool control_truncated{};
  std::size_t control_bytes{};
  int flags{};
};
/** @brief recvmsg 包装，在最终 awaiter 内保存消息头与 peer 存储。 */
template <class Addr, int Detailed>
class ReceiveDatagram : public AwaiterOptions<ReceiveDatagram<Addr, Detailed>> {
public:
  ReceiveDatagram(std::shared_ptr<io::detail::resource_state> resource,
                  std::span<char> buffer, int flags = 0,
                  std::span<std::byte> control = {})
      : resource_{std::move(resource)}, flags_{flags}, control_{control},
        capacity_{buffer.size()},
        vector_{buffer.empty() ? &discard_byte_ : buffer.data(),
                buffer.empty() ? 1 : buffer.size()},
        message_{[&] {
          msghdr message{};
          message.msg_name = address_.sockaddr();
          message.msg_namelen = Addr::capacity();
          message.msg_iov = &vector_;
          message.msg_iovlen = 1;
          message.msg_control = control.data();
          message.msg_controllen =
              static_cast<decltype(message.msg_controllen)>(control.size());
          return message;
        }()},
        operation_{io::recvmsg(resource_, &message_, flags)} {
    message_.msg_name = address_.sockaddr();
    message_.msg_namelen = Addr::capacity();
    message_.msg_iov = &vector_;
    message_.msg_iovlen = 1;
    message_.msg_control = control.data();
    message_.msg_controllen =
        static_cast<decltype(msghdr{}.msg_controllen)>(control.size());
  }
  ReceiveDatagram(const ReceiveDatagram &) = delete;
  /** @brief 仅在登记前移动，重建最终帧中的 msghdr、peer 和 iovec 指针。 */
  ReceiveDatagram(ReceiveDatagram &&other) noexcept
      : ReceiveDatagram{
            std::move(other.resource_),
            std::span<char>{static_cast<char *>(other.vector_.iov_base),
                            other.capacity_},
            other.flags_, other.control_} {
    static_cast<AwaiterOptions<ReceiveDatagram> &>(*this) =
        std::move(static_cast<AwaiterOptions<ReceiveDatagram> &>(other));
    owner_ = other.owner_;
  }
  /** @brief 组合操作内部复用所属方向租约，其 token 必须位于稳定协程帧。 */
  auto reservation(void *owner) & noexcept -> ReceiveDatagram & {
    owner_ = owner;
    return *this;
  }
  auto reservation(void *owner) && noexcept -> ReceiveDatagram && {
    owner_ = owner;
    return std::move(*this);
  }
  auto await_ready() noexcept { return operation_.await_ready(); }
  auto await_suspend(std::coroutine_handle<> continuation) {
    this->configure(operation_);
    operation_.reservation(owner_);
    return operation_.await_suspend(continuation);
  }
  auto await_resume() -> expected<
      std::conditional_t<Detailed == 1, DatagramMessage<Addr>,
                         std::conditional_t<Detailed == 2, std::size_t,
                                            std::pair<std::size_t, Addr>>>> {
    auto result = operation_.await_resume();
    if (!result)
      return std::unexpected{result.error()};
    if constexpr (requires { address_.set_length(message_.msg_namelen); })
      address_.set_length(message_.msg_namelen);
    // Linux MSG_TRUNC 可返回完整包长度；复制范围始终不超过提交 buffer。
    const auto copied = std::min(*result, capacity_);
    if constexpr (Detailed == 2)
      return copied;
    else if constexpr (Detailed == 1)
      return DatagramMessage<Addr>{copied,
                                   address_,
                                   (message_.msg_flags & MSG_TRUNC) != 0 ||
                                       *result > capacity_,
                                   (message_.msg_flags & MSG_CTRUNC) != 0,
                                   message_.msg_controllen,
                                   message_.msg_flags};
    else
      return std::pair{copied, address_};
  }

private:
  std::shared_ptr<io::detail::resource_state> resource_;
  void *owner_{};
  int flags_{};
  std::span<std::byte> control_;
  std::size_t capacity_;
  // Darwin 对零长度 iovec 的 recvmsg
  // 可直接返回而不消费消息；一字节帧内存储强制消息领取。
  char discard_byte_{};
  iovec vector_{};
  Addr address_{};
  msghdr message_{};
  decltype(io::recvmsg(
      std::declval<std::shared_ptr<io::detail::resource_state>>(), nullptr,
      0)) operation_;
};
/** @brief 拥有型数据报接收结果，实际初始化数据与来源地址一并返还。 */
template <class Addr> struct OwnedDatagram {
  io::io_buffer buffer;
  DatagramMessage<Addr> message;
};
/** @brief 消息型收包接口，peek 保留消息队列中的原始数据报。 */
template <class T, class Addr> struct ImplRecv {
  auto recv(std::span<char> buffer, int flags = 0) const noexcept {
    return ReceiveDatagram<Addr, 2>{static_cast<const T *>(this)->resource(),
                                    buffer, flags};
  }
  auto recv(io::borrowed_buffer buffer, int flags = 0) const noexcept {
    return recv(buffer.bytes, flags);
  }
  auto recv(io::io_buffer buffer, int flags = 0) const
      -> task<expected<io::io_transfer>> {
    return recv_owned_impl(static_cast<const T *>(this)->resource(),
                           std::move(buffer), flags);
  }
  auto peek(std::span<char> buffer) const noexcept {
    return recv(buffer, MSG_PEEK);
  }
  auto recv_from(std::span<char> buffer, int flags = 0) const noexcept {
    return ReceiveDatagram<Addr, false>{
        static_cast<const T *>(this)->resource(), buffer, flags};
  }
  auto recv_from(io::borrowed_buffer buffer, int flags = 0) const noexcept {
    return recv_from(buffer.bytes, flags);
  }
  auto recv_from(io::io_buffer buffer, int flags = 0) const
      -> task<expected<OwnedDatagram<Addr>>> {
    return recv_from_owned_impl(static_cast<const T *>(this)->resource(),
                                std::move(buffer), flags);
  }
  auto peek_from(std::span<char> buffer) const noexcept {
    return recv_from(buffer, MSG_PEEK);
  }
  auto recv_message(std::span<char> buffer, std::span<std::byte> control = {},
                    int flags = 0) const noexcept {
    return ReceiveDatagram<Addr, true>{static_cast<const T *>(this)->resource(),
                                       buffer, flags, control};
  }
  /** @brief 可移植批量收包：等待第一包，再即时接收已排队的数据，最大数量由 span
   * 限定。
   * @details 每项对应同下标的借用 buffer。任何错误保留已消费的包数量在
   * Error::progress。 kqueue 与 epoll 使用相同边界，零长度 buffer
   * 仍然消费一条合法数据报。
   */
  auto recv_many(std::span<std::span<char>> buffers) const
      -> task<expected<std::vector<DatagramMessage<Addr>>>> {
    return recv_many_impl(static_cast<const T *>(this)->resource(), buffers);
  }
  /** @brief 向 vector
   * 已初始化尾部接收单个消息，不把容量内的未初始化字节传给用户。 */
  auto recv_buf(std::vector<char> &buffer, std::size_t count = 65536) const
      -> task<expected<std::size_t>> {
    return recv_vector_append<2>(static_cast<const T *>(this)->resource(),
                                 buffer, count);
  }
  auto recv_buf_from(std::vector<char> &buffer, std::size_t count = 65536) const
      -> task<expected<std::pair<std::size_t, Addr>>> {
    return recv_vector_append<0>(static_cast<const T *>(this)->resource(),
                                 buffer, count);
  }
  auto recv_buf(io::read_buf &buffer) const -> task<expected<std::size_t>> {
    return recv_initialized<2>(static_cast<const T *>(this)->resource(),
                               buffer);
  }
  auto recv_buf_from(io::read_buf &buffer) const
      -> task<expected<std::pair<std::size_t, Addr>>> {
    return recv_initialized<0>(static_cast<const T *>(this)->resource(),
                               buffer);
  }
  [[nodiscard]] auto try_recv_buf(std::vector<char> &buffer,
                                  std::size_t count = 65536) const
      -> expected<std::size_t> {
    const auto initial = buffer.size();
    if (count > buffer.max_size() - initial)
      return std::unexpected{make_error(EOVERFLOW)};
    buffer.resize(initial + count);
    auto result = try_recv(std::span<char>{buffer}.subspan(initial));
    buffer.resize(initial + (result ? *result : 0));
    return result;
  }
  [[nodiscard]] auto try_recv_buf_from(std::vector<char> &buffer,
                                       std::size_t count = 65536) const
      -> expected<std::pair<std::size_t, Addr>> {
    const auto initial = buffer.size();
    if (count > buffer.max_size() - initial)
      return std::unexpected{make_error(EOVERFLOW)};
    buffer.resize(initial + count);
    auto result = try_recv_from(std::span<char>{buffer}.subspan(initial));
    buffer.resize(initial + (result ? result->first : 0));
    return result;
  }
  [[nodiscard]] auto try_recv_buf(io::read_buf &buffer) const
      -> expected<std::size_t> {
    auto result = try_recv(buffer.unfilled());
    if (result)
      buffer.advance(*result);
    return result;
  }
  [[nodiscard]] auto try_recv_buf_from(io::read_buf &buffer) const
      -> expected<std::pair<std::size_t, Addr>> {
    auto result = try_recv_from(buffer.unfilled());
    if (result)
      buffer.advance(result->first);
    return result;
  }
  [[nodiscard]] auto try_recv_message(std::span<char> buffer,
                                      std::span<std::byte> control = {},
                                      int flags = 0) const
      -> expected<DatagramMessage<Addr>> {
    return try_recv_message_impl(static_cast<const T *>(this)->resource(),
                                 buffer, control, flags);
  }
  [[nodiscard]] auto try_recv(std::span<char> buffer) const
      -> expected<std::size_t> {
    auto result = try_recv_message(buffer);
    if (!result)
      return std::unexpected{result.error()};
    return result->copied_bytes;
  }
  [[nodiscard]] auto try_peek(std::span<char> buffer) const
      -> expected<std::size_t> {
    auto result = try_recv_message(buffer, {}, MSG_PEEK);
    if (!result)
      return std::unexpected{result.error()};
    return result->copied_bytes;
  }
  [[nodiscard]] auto try_recv_from(std::span<char> buffer) const
      -> expected<std::pair<std::size_t, Addr>> {
    auto result = try_recv_message(buffer);
    if (!result)
      return std::unexpected{result.error()};
    return std::pair{result->copied_bytes, result->peer};
  }
  [[nodiscard]] auto try_peek_from(std::span<char> buffer) const
      -> expected<std::pair<std::size_t, Addr>> {
    auto result = try_recv_message(buffer, {}, MSG_PEEK);
    if (!result)
      return std::unexpected{result.error()};
    return std::pair{result->copied_bytes, result->peer};
  }

private:
  template <int Detailed>
  using buffer_result = std::conditional_t<Detailed == 2, std::size_t,
                                           std::pair<std::size_t, Addr>>;
  template <int Detailed>
  static auto copied_count(const buffer_result<Detailed> &result)
      -> std::size_t {
    if constexpr (Detailed == 2)
      return result;
    else
      return result.first;
  }
  /** @brief 静态协程保存资源快照和借用 buffer
   * 引用，避免延迟执行依赖包装对象地址。 */
  template <int Detailed>
  static auto
  recv_vector_append(std::shared_ptr<io::detail::resource_state> resource,
                     std::vector<char> &buffer, std::size_t count)
      -> task<expected<buffer_result<Detailed>>> {
    const auto initial = buffer.size();
    if (count > buffer.max_size() - initial)
      co_return std::unexpected{make_error(EOVERFLOW)};
    buffer.resize(initial + count);
    auto result = co_await ReceiveDatagram<Addr, Detailed>{
        std::move(resource), std::span<char>{buffer}.subspan(initial)};
    buffer.resize(initial + (result ? copied_count<Detailed>(*result) : 0));
    co_return result;
  }
  template <int Detailed>
  static auto
  recv_initialized(std::shared_ptr<io::detail::resource_state> resource,
                   io::read_buf &buffer)
      -> task<expected<buffer_result<Detailed>>> {
    auto result = co_await ReceiveDatagram<Addr, Detailed>{std::move(resource),
                                                           buffer.unfilled()};
    if (result)
      buffer.advance(copied_count<Detailed>(*result));
    co_return result;
  }
  static auto try_recv_message_impl(
      const std::shared_ptr<io::detail::resource_state> &resource,
      std::span<char> buffer, std::span<std::byte> control = {}, int flags = 0,
      void *owner = nullptr) -> expected<DatagramMessage<Addr>> {
    return io::detail::with_resource(
        resource, io::Interest::readable,
        [&]() -> expected<DatagramMessage<Addr>> {
          Addr address{};
          char
              discard_byte{}; // 零容量接口仍领取消息；复制结果依据用户原始容量裁剪为零。
          iovec vector{buffer.empty() ? &discard_byte : buffer.data(),
                       buffer.empty() ? 1 : buffer.size()};
          msghdr message{};
          message.msg_name = address.sockaddr();
          message.msg_namelen = Addr::capacity();
          message.msg_iov = &vector;
          message.msg_iovlen = 1;
          message.msg_control = control.data();
          message.msg_controllen =
              static_cast<decltype(message.msg_controllen)>(control.size());
          const auto result = ::recvmsg(resource->fd(), &message, flags);
          if (result < 0)
            return std::unexpected{make_error(errno)};
          if constexpr (requires { address.set_length(message.msg_namelen); })
            address.set_length(message.msg_namelen);
          return DatagramMessage<Addr>{
              std::min(static_cast<std::size_t>(result), buffer.size()),
              address,
              (message.msg_flags & MSG_TRUNC) != 0 ||
                  static_cast<std::size_t>(result) > buffer.size(),
              (message.msg_flags & MSG_CTRUNC) != 0,
              message.msg_controllen,
              message.msg_flags};
        },
        owner);
  }
  static auto
  recv_many_impl(std::shared_ptr<io::detail::resource_state> resource,
                 std::span<std::span<char>> buffers)
      -> task<expected<std::vector<DatagramMessage<Addr>>>> {
    if (buffers.empty())
      co_return std::unexpected{make_error(EINVAL)};
    char owner_identity{}; // 稳定帧内 token
                           // 覆盖首包挂起与后续即时收包，不允许其他读者插队。
    auto reservation = io::detail::reserve_direction(
        resource, io::Interest::readable, &owner_identity);
    if (!reservation)
      co_return std::unexpected{reservation.error()};
    std::vector<DatagramMessage<Addr>> results;
    results.reserve(buffers.size());
    auto first = co_await ReceiveDatagram<Addr, 1>{resource, buffers.front()}
                     .reservation(&owner_identity);
    if (!first)
      co_return std::unexpected{first.error()};
    results.push_back(*first);
    for (std::size_t index = 1; index < buffers.size(); ++index) {
      // 原生后端的后续消息仍交给 RECVMSG SQE/CQE，MSG_DONTWAIT
      // 保持即时批量语义。 readiness 后端复用显式同步 try
      // 路径；原生路径不借它偷偷消费 socket 数据。
      auto next =
          resource->owner->supports_native(io::detail::operation_kind::recvmsg)
              ? co_await ReceiveDatagram<Addr, 1>{resource, buffers[index],
                                                  MSG_DONTWAIT}
                    .reservation(&owner_identity)
              : try_recv_message_impl(resource, buffers[index], {}, 0,
                                      &owner_identity);
      if (!next) {
        if (next.error().value() == EAGAIN ||
            next.error().value() == EWOULDBLOCK)
          break;
        co_return std::unexpected{
            Error{next.error().value(), results.size(), next.error().domain()}};
      }
      results.push_back(*next);
    }
    co_return results;
  }
  static auto
  recv_owned_impl(std::shared_ptr<io::detail::resource_state> resource,
                  io::io_buffer buffer, int flags)
      -> task<expected<io::io_transfer>> {
    auto result = co_await ReceiveDatagram<Addr, 2>{
        std::move(resource), buffer.writable_bytes(), flags};
    if (!result)
      co_return std::unexpected{result.error()};
    buffer.set_size(*result);
    co_return io::io_transfer{std::move(buffer), *result};
  }
  static auto
  recv_from_owned_impl(std::shared_ptr<io::detail::resource_state> resource,
                       io::io_buffer buffer, int flags)
      -> task<expected<OwnedDatagram<Addr>>> {
    auto result = co_await ReceiveDatagram<Addr, 1>{
        std::move(resource), buffer.writable_bytes(), flags};
    if (!result)
      co_return std::unexpected{result.error()};
    buffer.set_size(result->copied_bytes);
    co_return OwnedDatagram<Addr>{std::move(buffer), *result};
  }
};
} // namespace faio::net::detail
#endif
