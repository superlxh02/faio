#ifndef FAIO_DETAIL_NET_COMMON_STREAM_WRITE_HPP
#define FAIO_DETAIL_NET_COMMON_STREAM_WRITE_HPP
#include "faio/detail/net/common/platform.hpp"
#include "faio/detail/common/concepts.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/buffer.hpp"
#include "faio/detail/net/common/address.hpp"
#include "faio/detail/net/common/awaiter_options.hpp"
#include "faio/detail/net/common/socket.hpp"
#include <array>
#include <climits>
#include <span>
#include <variant>
#include <vector>

namespace faio::net::detail {
/** @brief 聚集发送 awaiter；借用数据，持有 iovec 与 msghdr，不复制 payload。 */
template <std::size_t N>
class VectoredWrite : public AwaiterOptions<VectoredWrite<N>> {
 public:
  template <class... Buffers>
  VectoredWrite(std::shared_ptr<io::detail::resource_state> resource, Buffers&&... buffers)
      : resource_{std::move(resource)},
        vectors_{iovec{const_cast<char*>(std::span<const char>{buffers}.data()),
                       std::span<const char>{buffers}.size_bytes()}...},
        message_{[&] {
          msghdr message{};
          message.msg_iov = vectors_.data();
          message.msg_iovlen = N;
          return message;
        }()},
        operation_{io::sendmsg(resource_, &message_, no_signal_flags)} {
    operation_.empty_success();  // 成员原位配置，避免 && builder 按值移动整份请求。
    message_.msg_iov = vectors_.data();
    message_.msg_iovlen = N;
  }

  VectoredWrite(const VectoredWrite&) = delete;

  /** @brief 提交前移动时重建消息指针，不让后端借用已销毁临时 awaiter。 */
  VectoredWrite(VectoredWrite&& other) noexcept
      : AwaiterOptions<VectoredWrite<N>>{std::move(other)},
        resource_{std::move(other.resource_)},
        vectors_{other.vectors_},
        message_{[&] {
          msghdr message{};
          message.msg_iov = vectors_.data();
          message.msg_iovlen = N;
          return message;
        }()},
        operation_{io::sendmsg(resource_, &message_, no_signal_flags)} {
    operation_.empty_success();  // 成员原位配置，避免 && builder 按值移动整份请求。
    message_.msg_iov = vectors_.data();
    message_.msg_iovlen = N;
  }

  auto await_ready() noexcept { return operation_.await_ready(); }

  auto await_suspend(std::coroutine_handle<> continuation) {
    this->configure(operation_);
    return operation_.await_suspend(continuation);
  }

  auto await_resume() noexcept { return operation_.await_resume(); }

 private:
  std::shared_ptr<io::detail::resource_state> resource_;
  std::array<iovec, N> vectors_{};
  msghdr message_{};
  decltype(io::sendmsg(
      std::declval<std::shared_ptr<io::detail::resource_state>>(), nullptr, 0)) operation_;
};

class BorrowedVectoredWrite : public AwaiterOptions<BorrowedVectoredWrite> {
 public:
  BorrowedVectoredWrite(std::shared_ptr<io::detail::resource_state> resource,
                        std::span<const iovec> vectors)
      : resource_{std::move(resource)},
        message_{[&] {
          msghdr message{};
          message.msg_iov = const_cast<iovec*>(vectors.data());
          message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(vectors.size());
          return message;
        }()},
        operation_{io::sendmsg(resource_, &message_, no_signal_flags)} {
    operation_.empty_success();  // 成员原位配置，避免 && builder 按值移动整份请求。
    // POSIX msghdr 的非 const 指针并不表示 sendmsg 会修改 iovec 或 payload。
    message_.msg_iov = const_cast<iovec*>(vectors.data());
    message_.msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(vectors.size());
  }

  BorrowedVectoredWrite(const BorrowedVectoredWrite&) = delete;

  BorrowedVectoredWrite(BorrowedVectoredWrite&& other) noexcept
      : AwaiterOptions<BorrowedVectoredWrite>{std::move(other)},
        resource_{std::move(other.resource_)},
        message_{other.message_},
        operation_{io::sendmsg(resource_, &message_, no_signal_flags)} {
    operation_.empty_success();  // 成员原位配置，避免 && builder 按值移动整份请求。
  }

  auto await_ready() noexcept { return operation_.await_ready(); }

  auto await_suspend(std::coroutine_handle<> continuation) {
    this->configure(operation_);
    return operation_.await_suspend(continuation);
  }

  auto await_resume() noexcept { return operation_.await_resume(); }

 private:
  std::shared_ptr<io::detail::resource_state> resource_;
  msghdr message_{};
  decltype(io::sendmsg(
      std::declval<std::shared_ptr<io::detail::resource_state>>(), nullptr, 0)) operation_;
};

/** @brief 字节流的安全零拷贝发送；不支持时使用明确的普通发送路径。
 * @details 构造时仅保存资源与借用范围，在 await_suspend 根据实际 IO domain
 * 能力选择请求。 IP stream 使用原生 SEND_ZC；Unix stream 和 readiness
 * 后端使用普通 SEND。 原生路径由统一引擎等待最终 buffer-release
 * 通知，返回前不会再借用 payload。
 *          返回值是本次实际进度，仍允许短写；零长度范围使用流式无操作语义。
 */
class ZeroCopyWrite : public AwaiterOptions<ZeroCopyWrite> {
  using Send = decltype(io::send(std::declval<io::detail::resource_ptr>(), nullptr, 0, 0));
  using SendZC = decltype(io::send_zc(std::declval<io::detail::resource_ptr>(), nullptr, 0, 0));

 public:
  ZeroCopyWrite(io::detail::resource_ptr resource,
                std::span<const char> buffer,
                bool ip_stream) noexcept
      : resource_{std::move(resource)}, buffer_{buffer}, ip_stream_{ip_stream} {}

  ZeroCopyWrite(const ZeroCopyWrite&) = delete;

  auto operator=(const ZeroCopyWrite&) -> ZeroCopyWrite& = delete;

  ZeroCopyWrite(ZeroCopyWrite&&) noexcept = default;

  auto operator=(ZeroCopyWrite&&) -> ZeroCopyWrite& = delete;

  bool await_ready() const noexcept { return false; }  // 选择后端也不在构造/ready 阶段提交。

  bool await_suspend(std::coroutine_handle<> continuation) noexcept {
    auto* domain = resource_ ? resource_->owner.get() : nullptr;  // 已绑定资源保持固定归属。
    auto current =
        domain ? io::io_context{} : io::io_context::current();  // lazy 对象使用首次提交的默认域。
    if (!domain && current)
      domain = current.domain().get();  // 当前域只用于能力判定，实际绑定由统一桥完成。
    if (ip_stream_ && domain && domain->capabilities().zero_copy) {
      auto& operation =
          operation_.emplace<2>(resource_, buffer_.data(), buffer_.size(), no_signal_flags, 0);
      operation.empty_success();   // TCP 空范围不产生一条数据报，也不等待通知。
      this->configure(operation);  // 绝对 deadline 随提交前移动保存，不延长超时时间。
      return operation.await_suspend(continuation);  // NOTIF 排空由 native operation 状态机完成。
    }
    auto& operation =
        operation_.emplace<1>(resource_, buffer_.data(), buffer_.size(), no_signal_flags);
    operation.empty_success();                     // fallback 仍验证资源、方向和停止状态。
    this->configure(operation);                    // 两条路径采用同一取消与 deadline 语义。
    return operation.await_suspend(continuation);  // 不额外创建组合 task 或复制 payload。
  }

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (const auto* operation = std::get_if<2>(&operation_))
      return operation->await_resume();
    if (const auto* operation = std::get_if<1>(&operation_))
      return operation->await_resume();
    return std::unexpected{make_error(EINVAL)};  // 未提交的非法手动调用不会读取空 variant。
  }

 private:
  io::detail::resource_ptr resource_;  // lease 保持实际 domain 与句柄控制块有效。
  std::span<const char> buffer_;       // 借用 payload，调用者在最终结果返回前不得修改或销毁。
  bool ip_stream_{};                   // 原生 SEND_ZC 的网络协议限制，不将 Unix socket 当作 TCP。
  std::variant<std::monostate, Send, SendZC> operation_;  // 仅所选请求建立操作状态。
};

/** @brief TCP/Unix stream 写方向，默认不额外缓存数据，直接服从内核背压。 */
template <class T>
struct ImplStreamWrite {
  auto write(std::span<const char> buffer) const noexcept {
    auto operation = io::send(
        static_cast<const T*>(this)->resource(), buffer.data(), buffer.size(), no_signal_flags);
    operation.empty_success();  // 左值配置不移动大型 io_request，return 保持 NRVO。
    return operation;
  }

  auto write(io::borrowed_const_buffer buffer) const noexcept { return write(buffer.bytes); }

  /** @brief 移入发送缓冲区，返回 buffer
   * 和本次实际进度，短写可继续发送剩余范围。 */
  auto write(io::io_buffer buffer) const -> task<expected<io::io_transfer>> {
    return write_buffer_owned(static_cast<const T*>(this)->resource(), std::move(buffer));
  }

  auto write(io::shared_const_buffer buffer) const -> task<expected<std::size_t>> {
    return write_buffer_shared(static_cast<const T*>(this)->resource(), std::move(buffer));
  }

  /** @brief 优先采用 IP stream 原生零拷贝；能力不支持时安全降级普通发送。
   * @details 即使发送结果已产生，原生路径仍等待 buffer-release 通知后返回。
   */
  auto write_zc(std::span<const char> buffer) const noexcept {
    constexpr bool ip_stream = [] {
      if constexpr (requires { typename T::address_type; })
        return std::same_as<typename T::address_type, SocketAddr>;
      else
        return false;
    }();
    return ZeroCopyWrite{static_cast<const T*>(this)->resource(), buffer, ip_stream};
  }

  auto write_vectored(std::span<const iovec> vectors) const noexcept {
    return BorrowedVectoredWrite{static_cast<const T*>(this)->resource(), vectors};
  }

  template <class... Buffers>
    requires(sizeof...(Buffers) > 0 && (constructible_to_char_slice<Buffers> && ...))
  auto write_vectored(Buffers&&... buffers) const noexcept {
    return VectoredWrite<sizeof...(Buffers)>{static_cast<const T*>(this)->resource(),
                                             std::forward<Buffers>(buffers)...};
  }

  template <class... Buffers>
  auto write_v(Buffers&&... buffers) const noexcept {
    return write_vectored(std::forward<Buffers>(buffers)...);
  }

  [[nodiscard]] auto try_write(std::span<const char> buffer) const -> expected<std::size_t> {
    auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::Interest::writable, [&]() -> expected<std::size_t> {
          if (buffer.empty())
            return std::size_t{0};
          decltype(socket_send(0, nullptr, 0, 0)) result{};
          for (int attempt = 0; attempt < 8; ++attempt) {
            result = socket_send(object->fd(), buffer.data(), buffer.size(), no_signal_flags);
            if (result >= 0
                || (socket_error().value() != EINTR
#if defined(_WIN32)
                    && socket_error().value() != WSAEINTR
#endif
                    ))
              break;
          }
          if (result < 0)
            return std::unexpected{socket_error()};
          return static_cast<std::size_t>(result);
        });
  }

  [[nodiscard]] auto try_write_vectored(std::span<const iovec> vectors) const
      -> expected<std::size_t> {
    auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::Interest::writable, [&]() -> expected<std::size_t> {
          if (vectors.size() > IOV_MAX)
            return std::unexpected{make_error(EINVAL)};
          msghdr message{};
          message.msg_iov = const_cast<iovec*>(vectors.data());
          message.msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(vectors.size());
          const auto result = socket_sendmsg(object->fd(), &message, no_signal_flags);
          if (result < 0)
            return std::unexpected{socket_error()};
          return static_cast<std::size_t>(result);
        });
  }

  /** @brief 完整发送；短写继续推进，非空 buffer 的零进度返回 WriteZero。 */
  task<expected<void>> write_all(std::span<const char> buffer) const {
    return write_all_impl(static_cast<const T*>(this)->resource(), buffer);
  }

  task<expected<std::size_t>> write_owned(std::vector<char> buffer) const {
    return write_vector_owned(static_cast<const T*>(this)->resource(), std::move(buffer));
  }

  task<expected<void>> write_all_buf(std::span<const char> buffer) const {
    return write_all(buffer);
  }

  /** @brief 无用户态写缓存，单次 write 完成即提交到内核。 */
  task<expected<void>> flush() const { co_return expected<void>{}; }

 private:
  /** @brief 调用时捕获资源，拥有型 vector 的存储覆盖整个延迟启动与发送过程。 */
  static task<expected<std::size_t>> write_vector_owned(
      std::shared_ptr<io::detail::resource_state> resource, std::vector<char> buffer) {
    auto operation = io::send(std::move(resource), buffer.data(), buffer.size(), no_signal_flags);
    operation.empty_success();
    co_return co_await operation;
  }

  static task<expected<io::io_transfer>> write_buffer_owned(
      std::shared_ptr<io::detail::resource_state> resource, io::io_buffer buffer) {
    auto operation = io::send(std::move(resource), buffer.data(), buffer.size(), no_signal_flags);
    operation.empty_success();
    auto result = co_await operation;
    if (!result)
      co_return std::unexpected{result.error()};
    co_return io::io_transfer{std::move(buffer), *result};
  }

  static task<expected<std::size_t>> write_buffer_shared(
      std::shared_ptr<io::detail::resource_state> resource, io::shared_const_buffer buffer) {
    auto operation =
        io::send(std::move(resource), buffer.bytes().data(), buffer.size(), no_signal_flags);
    operation.empty_success();
    co_return co_await operation;
  }

  static task<expected<void>> write_all_impl(std::shared_ptr<io::detail::resource_state> resource,
                                             std::span<const char> buffer) {
    if (buffer.empty())
      co_return expected<void>{};
    char owner_identity{};  // 地址位于稳定组合协程帧，仅作为方向 reservation
    // 身份。
    if (!resource)
      co_return std::unexpected{make_error(EBADF)};
    // 仅先建立 RAII 释放责任；首次 send 在现有域锁内取得租约，省掉独立 reserve
    // 锁。 未绑定资源仍由正常 await_suspend 首次绑定；所有失败出口都按身份安全
    // unreserve。 按值 resource 参数覆盖整个组合帧，且下面始终传副本给
    // send，不移动/清空它。
    // 局部借用守卫先于参数销毁，因此无需为相同控制块再增减一次原子引用计数。
    io::detail::borrowed_direction_lease reservation{
        *resource, io::Interest::writable, &owner_identity};
    std::size_t transferred{};
    bool first_operation = true;
    while (!buffer.empty()) {
      {
        auto operation = io::send(resource, buffer.data(), buffer.size(), no_signal_flags);
        if (first_operation) {
          operation.establish_reservation(&owner_identity);  // 首发送在原有域锁内取得组合方向。
          first_operation = false;
        } else {
          operation.reservation(&owner_identity);  // 短写承接同一租约，禁止重新取得或释放方向。
        }
        operation.empty_success();  // 所有配置使用 & setter，免去每个 && builder
        // 的整份请求移动。
        auto result = co_await operation;  // 局部 awaiter
        // 在协程帧内稳定，桥只借用它直到完成。
        if (!result)
          co_return std::unexpected{Error{result.error().value(),
                                          transferred + result.error().progress(),
                                          result.error().domain()}};
        if (*result == 0)
          co_return std::unexpected{Error{Error::WriteZero, transferred, error_domain::faio}};
        transferred += *result;
        buffer = buffer.subspan(*result);
      }  // 真实 IO 已完成，先释放 awaiter/stop
      // callback，再单独检查协作让出预算。
      co_await this_coro::yield_if_needed();
    }
    co_return expected<void>{};
  }
};
}  // namespace faio::net::detail
#endif
