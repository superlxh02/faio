/**
 * @file faio/detail/net/base/datagram_send.hpp
 * @brief 网络地址、socket 与传输协议：数据报发送与负载租约。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#ifndef FAIO_DETAIL_NET_BASE_DATAGRAM_SEND_HPP
#define FAIO_DETAIL_NET_BASE_DATAGRAM_SEND_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/base/buffer.hpp"
#include "faio/detail/net/base/awaiter_options.hpp"
#include "faio/detail/net/base/socket.hpp"
#include <climits>
#include <cstddef>
#include <optional>
#include <span>

namespace faio::net::detail {
/** @brief 地址按值持有的 sendmsg awaiter，临时 SocketAddr 可安全用于 co_await。
 */
template <class Addr>
class SendDatagram : public AwaiterOptions<SendDatagram<Addr>> {
 public:
  SendDatagram(std::shared_ptr<io::detail::resource_state> resource,
               std::span<const char> buffer,
               Addr address)
      : resource_{std::move(resource)},
        address_{std::move(address)},
        vector_{const_cast<char*>(buffer.data()), buffer.size()},
        message_{[&] {
          msghdr message{};
          message.msg_name = address_->sockaddr();
          message.msg_namelen = address_->length();
          message.msg_iov = &vector_;
          message.msg_iovlen = 1;
          return message;
        }()},
        operation_{io::sendmsg(resource_, &message_, no_signal_flags)} {
    message_.msg_name = address_->sockaddr();
    message_.msg_namelen = address_->length();
    message_.msg_iov = &vector_;
    message_.msg_iovlen = 1;
  }

  SendDatagram(std::shared_ptr<io::detail::resource_state> resource,
               std::span<const iovec> vectors,
               std::optional<Addr> address,
               std::span<const std::byte> control,
               int flags)
      : resource_{std::move(resource)},
        flags_{flags},
        address_{std::move(address)},
        message_{[&] {
          msghdr message{};
          message.msg_name = address_ ? address_->sockaddr() : nullptr;
          message.msg_namelen = address_ ? address_->length() : 0;
          message.msg_iov = const_cast<iovec*>(vectors.data());
          message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(vectors.size());
          message.msg_control = const_cast<std::byte*>(control.data());
          message.msg_controllen = static_cast<decltype(message.msg_controllen)>(control.size());
          return message;
        }()},
        operation_{io::sendmsg(resource_, &message_, flags | no_signal_flags)} {
    if (address_) {
      message_.msg_name = address_->sockaddr();
      message_.msg_namelen = address_->length();
    }
    message_.msg_iov = const_cast<iovec*>(vectors.data());
    message_.msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(vectors.size());
    message_.msg_control = const_cast<std::byte*>(control.data());
    message_.msg_controllen = static_cast<decltype(msghdr{}.msg_controllen)>(control.size());
  }

  SendDatagram(const SendDatagram&) = delete;

  /** @brief 登记前移动，区分内置单个 iovec 与调用者借用的 iovec 数组。 */
  SendDatagram(SendDatagram&& other) noexcept
      : AwaiterOptions<SendDatagram<Addr>>{std::move(other)},
        resource_{std::move(other.resource_)},
        owner_{other.owner_},
        flags_{other.flags_},
        address_{std::move(other.address_)},
        vector_{other.vector_},
        message_{[&] {
          msghdr message{};
          message.msg_name = address_ ? address_->sockaddr() : nullptr;
          message.msg_namelen = other.message_.msg_namelen;
          message.msg_iov =
              other.message_.msg_iov == &other.vector_ ? &vector_ : other.message_.msg_iov;
          message.msg_iovlen = other.message_.msg_iovlen;
          message.msg_control = other.message_.msg_control;
          message.msg_controllen = other.message_.msg_controllen;
          return message;
        }()},
        operation_{io::sendmsg(resource_, &message_, flags_ | no_signal_flags)} {
    if (address_)
      message_.msg_name = address_->sockaddr();
    if (other.message_.msg_iov == &other.vector_)
      message_.msg_iov = &vector_;
  }

  /// @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。
  auto reservation(void* owner) & noexcept -> SendDatagram& {
    owner_ = owner;
    return *this;
  }

  /// @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。
  auto reservation(void* owner) && noexcept -> SendDatagram&& {
    owner_ = owner;
    return std::move(*this);
  }

  /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
  auto await_ready() noexcept { return operation_.await_ready(); }

  /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
  auto await_suspend(std::coroutine_handle<> continuation) {
    this->configure(operation_);
    operation_.reservation(owner_);
    return operation_.await_suspend(continuation);
  }

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  auto await_resume() noexcept { return operation_.await_resume(); }

 private:
  std::shared_ptr<io::detail::resource_state> resource_;  ///< 共享资源控制块租约，跨挂起固定句柄身份及 IO 域归属。
  void* owner_{};  ///< 组合操作的稳定身份，用于验证读写方向的排他预留。
  int flags_{};  ///< 本次原生操作的标志位，按对应系统调用语义解释。
  std::optional<Addr> address_;  ///< 拥有的本地或对端 socket 地址，提交期间不借用临时对象。
  iovec vector_{};  ///< 分散聚集 IO 的缓冲区描述，负载仍须由调用方保持有效。
  msghdr message_{};  ///< 稳定保存的消息描述及缓冲区引用，安全完成前不得迁移。
  decltype(io::sendmsg(
      std::declval<std::shared_ptr<io::detail::resource_state>>(), nullptr, 0)) operation_;  ///< 地址稳定的底层异步操作，当前包装负责其启动、等待与结果交付。
};

/** @brief 批量发送的一项；借用 payload，可为 connected socket 省略地址。 */
template <class Addr>
struct DatagramSend {
  std::span<const char> bytes;  ///< 发送时借用的只读字节区，完成前保持有效。
  std::optional<Addr> peer;  ///< 拥有的本地或对端 socket 地址，提交期间不借用临时对象。
};

/** @brief 消息型发送，不使用 write_all 合并/拆分数据报。 */
template <class T, class Addr>
struct ImplSend {
  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto send(std::span<const char> buffer) const noexcept {
    return io::send(
        static_cast<const T*>(this)->resource(), buffer.data(), buffer.size(), no_signal_flags);
  }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto send(io::borrowed_const_buffer buffer) const noexcept { return send(buffer.bytes); }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto send(io::io_buffer buffer) const -> task<expected<io::io_transfer>> {
    return send_owned_impl(
        static_cast<const T*>(this)->resource(), std::move(buffer), std::nullopt);
  }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto send(io::shared_const_buffer buffer) const -> task<expected<std::size_t>> {
    return send_shared_impl(static_cast<const T*>(this)->resource(), std::move(buffer));
  }

  auto send_to(std::span<const char> buffer, Addr address) const noexcept {
    return SendDatagram<Addr>{static_cast<const T*>(this)->resource(), buffer, std::move(address)};
  }

  auto send_to(io::borrowed_const_buffer buffer, Addr address) const noexcept {
    return send_to(buffer.bytes, std::move(address));
  }

  auto send_to(io::io_buffer buffer, Addr address) const -> task<expected<io::io_transfer>> {
    return send_owned_impl(
        static_cast<const T*>(this)->resource(), std::move(buffer), std::move(address));
  }

  /** @brief 有界批量发送，逐消息保持边界；失败时 progress
   * 表示已发出的消息数量。 */
  auto send_many(std::span<const DatagramSend<Addr>> messages) const
      -> task<expected<std::size_t>> {
    return send_many_impl(static_cast<const T*>(this)->resource(), messages);
  }

  auto send_message(std::span<const iovec> vectors,
                    std::optional<Addr> address = {},
                    std::span<const std::byte> control = {},
                    int flags = 0) const noexcept {
    return SendDatagram<Addr>{
        static_cast<const T*>(this)->resource(), vectors, std::move(address), control, flags};
  }

  [[nodiscard]] auto try_send(std::span<const char> buffer) const -> expected<std::size_t> {
    auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::Interest::writable, [&]() -> expected<std::size_t> {
          const auto result =
              socket_send(object->fd(), buffer.data(), buffer.size(), no_signal_flags);
          if (result < 0)
            return std::unexpected{socket_error()};
          return static_cast<std::size_t>(result);
        });
  }

  [[nodiscard]] auto try_send_to(std::span<const char> buffer, const Addr& address) const
      -> expected<std::size_t> {
    auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::Interest::writable, [&]() -> expected<std::size_t> {
          const auto result = socket_sendto(object->fd(),
                                            buffer.data(),
                                            buffer.size(),
                                            no_signal_flags,
                                            address.sockaddr(),
                                            address.length());
          if (result < 0)
            return std::unexpected{socket_error()};
          return static_cast<std::size_t>(result);
        });
  }

  [[nodiscard]] auto try_send_message(std::span<const iovec> vectors,
                                      std::optional<Addr> address = {},
                                      std::span<const std::byte> control = {},
                                      int flags = 0) const -> expected<std::size_t> {
    auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::Interest::writable, [&]() -> expected<std::size_t> {
          if (vectors.size() > IOV_MAX)
            return std::unexpected{make_error(EINVAL)};
          msghdr message{};
          if (address) {
            message.msg_name = address->sockaddr();
            message.msg_namelen = address->length();
          }
          message.msg_iov = const_cast<iovec*>(vectors.data());
          message.msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(vectors.size());
          message.msg_control = const_cast<std::byte*>(control.data());
          message.msg_controllen = static_cast<decltype(msghdr{}.msg_controllen)>(control.size());
          const auto result = socket_sendmsg(object->fd(), &message, flags | no_signal_flags);
          if (result < 0)
            return std::unexpected{socket_error()};
          return static_cast<std::size_t>(result);
        });
  }

 private:
  /** @brief 整个批次持有写方向 token，短挂起或协作让步期间仍保持调用顺序。 */
  static auto send_many_impl(std::shared_ptr<io::detail::resource_state> resource,
                             std::span<const DatagramSend<Addr>> messages)
      -> task<expected<std::size_t>> {
    if (messages.empty())
      co_return std::size_t{0};
    char owner_identity{};
    auto reservation =
        io::detail::reserve_direction(resource, io::Interest::writable, &owner_identity);
    if (!reservation)
      co_return std::unexpected{reservation.error()};
    std::size_t completed{};
    for (const auto& message : messages) {
      auto result =
          message.peer
              ? co_await SendDatagram<Addr>{resource, message.bytes, *message.peer}.reservation(
                    &owner_identity)
              : co_await io::send(
                    resource, message.bytes.data(), message.bytes.size(), no_signal_flags)
                    .reservation(&owner_identity);
      if (!result)
        co_return std::unexpected{
            Error{result.error().value(), completed, result.error().domain()}};
      ++completed;
      co_await this_coro::yield_if_needed();
    }
    co_return completed;
  }

  static auto send_owned_impl(std::shared_ptr<io::detail::resource_state> resource,
                              io::io_buffer buffer,
                              std::optional<Addr> address) -> task<expected<io::io_transfer>> {
    auto result = address
                      ? co_await SendDatagram<Addr>{resource, buffer.bytes(), *address}
                      : co_await io::send(resource, buffer.data(), buffer.size(), no_signal_flags);
    if (!result)
      co_return std::unexpected{result.error()};
    co_return io::io_transfer{std::move(buffer), *result};
  }

  static auto send_shared_impl(std::shared_ptr<io::detail::resource_state> resource,
                               io::shared_const_buffer buffer) -> task<expected<std::size_t>> {
    co_return co_await io::send(
        std::move(resource), buffer.bytes().data(), buffer.size(), no_signal_flags);
  }
};
}
#endif
