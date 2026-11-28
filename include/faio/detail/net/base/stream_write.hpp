/**
 * @file faio/detail/net/base/stream_write.hpp
 * @brief 字节流完整写入与零拷贝。
 */
#ifndef FAIO_DETAIL_NET_BASE_STREAM_WRITE_HPP
#define FAIO_DETAIL_NET_BASE_STREAM_WRITE_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/io/awaiter/send.hpp"
#include "faio/detail/io/awaiter/sendmsg.hpp"
#include "faio/detail/io/core/direction_lease.hpp"
#include "faio/detail/common/concepts.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/base/buffer.hpp"
#include "faio/detail/net/base/address_value.hpp"
#include "faio/detail/net/base/awaiter_options.hpp"
#include "faio/detail/net/base/socket.hpp"
#include <array>
#include <climits>
#include <span>
#include <variant>
#include <vector>

namespace faio::net::detail {
/**  @brief 聚集发送 awaiter；借用数据，持有 iovec 与 msghdr，不复制 payload。 */
template <std::size_t N>
class vectored_write : public awaiter_options<vectored_write<N>> {
 public:
  /** @brief 接管源对象的状态和资源租约。 */
  template <class... Buffers>
  vectored_write(std::shared_ptr<io::detail::resource_state> resource, Buffers&&... buffers)
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
    operation_.empty_success();
    message_.msg_iov = vectors_.data();
    message_.msg_iovlen = N;
  }

  /** @brief 禁止复制此对象。 */
  vectored_write(const vectored_write&) = delete;

  /**  @brief 提交前移动时重建消息指针，不让后端借用已销毁临时 awaiter。 */
  vectored_write(vectored_write&& other) noexcept
      : awaiter_options<vectored_write<N>>{std::move(other)},
        resource_{std::move(other.resource_)},
        vectors_{other.vectors_},
        message_{[&] {
          msghdr message{};
          message.msg_iov = vectors_.data();
          message.msg_iovlen = N;
          return message;
        }()},
        operation_{io::sendmsg(resource_, &message_, no_signal_flags)} {
    operation_.empty_success();
    message_.msg_iov = vectors_.data();
    message_.msg_iovlen = N;
  }

  /** @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。 */
  auto await_ready() noexcept { return operation_.await_ready(); }

  /** @brief 登记恢复目标，按等待协议交接挂起与完成责任。 */
  template <class Promise>
  auto await_suspend(std::coroutine_handle<Promise> continuation) {
    this->configure(operation_);
    return operation_.await_suspend(continuation);
  }

  /** @brief 完成交接后领取结果，并按接口约定传播失败或取消。 */
  auto await_resume() noexcept { return operation_.await_resume(); }

 private:
  std::shared_ptr<io::detail::resource_state>
      resource_; /**< 共享资源控制块的租约，在挂起期间保持句柄身份及所属 I/O 域不变。 */
  std::array<iovec, N> vectors_{}; /**< 分散/聚集 I/O 的缓冲区描述，调用方仍须保持负载有效。 */
  msghdr message_{};               /**< 稳定保存的消息描述及缓冲区引用，安全完成前不得迁移。 */
  decltype(io::sendmsg(
      std::declval<std::shared_ptr<io::detail::resource_state>>(),
      nullptr,
      0)) operation_; /**< 地址固定的底层异步操作，由包装器负责启动、等待和交付结果。 */
};

/** @brief 借用 iovec 描述表的聚集写入等待器。 */
class borrowed_vectored_write : public awaiter_options<borrowed_vectored_write> {
 public:
  /** @brief 借用聚集写入描述表，调用方保证其存活至完成。 */
  borrowed_vectored_write(std::shared_ptr<io::detail::resource_state> resource,
                          std::span<const iovec> vectors)
      : resource_{std::move(resource)},
        message_{[&] {
          msghdr message{};
          message.msg_iov = const_cast<iovec*>(vectors.data());
          message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(vectors.size());
          return message;
        }()},
        operation_{io::sendmsg(resource_, &message_, no_signal_flags)} {
    operation_.empty_success();
    message_.msg_iov = const_cast<iovec*>(vectors.data());
    message_.msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(vectors.size());
  }

  /** @brief 禁止复制此对象。 */
  borrowed_vectored_write(const borrowed_vectored_write&) = delete;

  /** @brief 接管源对象的状态和资源租约。 */
  borrowed_vectored_write(borrowed_vectored_write&& other) noexcept
      : awaiter_options<borrowed_vectored_write>{std::move(other)},
        resource_{std::move(other.resource_)},
        message_{other.message_},
        operation_{io::sendmsg(resource_, &message_, no_signal_flags)} {
    operation_.empty_success();
  }

  /** @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。 */
  auto await_ready() noexcept { return operation_.await_ready(); }

  /** @brief 登记恢复目标，按等待协议交接挂起与完成责任。 */
  template <class Promise>
  auto await_suspend(std::coroutine_handle<Promise> continuation) {
    this->configure(operation_);
    return operation_.await_suspend(continuation);
  }

  /** @brief 完成交接后领取结果，并按接口约定传播失败或取消。 */
  auto await_resume() noexcept { return operation_.await_resume(); }

 private:
  std::shared_ptr<io::detail::resource_state>
      resource_;     /**< 共享资源控制块的租约，在挂起期间保持句柄身份及所属 I/O 域不变。 */
  msghdr message_{}; /**< 稳定保存的消息描述及缓冲区引用，安全完成前不得迁移。 */
  decltype(io::sendmsg(
      std::declval<std::shared_ptr<io::detail::resource_state>>(),
      nullptr,
      0)) operation_; /**< 地址固定的底层异步操作，由包装器负责启动、等待和交付结果。 */
};

/**
 *  @brief 字节流的安全零拷贝发送；不支持时使用明确的普通发送路径。
 * @details 构造时仅保存资源与借用范围，在 await_suspend 根据实际 IO domain
 * 能力选择请求。 IP stream 使用原生 SEND_ZC；Unix stream 和 readiness
 * 后端使用普通 SEND。 原生路径由统一引擎等待最终 buffer-release
 * 通知，返回前不会再借用 payload。
 *          返回值是本次实际进度，仍允许短写；零长度范围使用流式无操作语义。
 */
class zero_copy_write : public awaiter_options<zero_copy_write> {
  using send_awaiter = decltype(io::send(
      std::declval<io::detail::resource_ptr>(), nullptr, 0, 0)); /**< 普通发送等待器类型。 */
  using send_zc_awaiter =
      decltype(io::send_zc(std::declval<io::detail::resource_ptr>(),
                           nullptr,
                           0,
                           0)); /**< 零拷贝发送等待器类型，完成须包含负载释放阶段。 */

 public:
  /** @brief 持有零拷贝写入所需的负载与资源租约。 */
  zero_copy_write(io::detail::resource_ptr resource,
                  std::span<const char> buffer,
                  bool ip_stream) noexcept
      : resource_{std::move(resource)}, buffer_{buffer}, ip_stream_{ip_stream} {}

  /** @brief 禁止复制此对象。 */
  zero_copy_write(const zero_copy_write&) = delete;

  /** @brief 禁止复制赋值。 */
  auto operator=(const zero_copy_write&) -> zero_copy_write& = delete;

  /** @brief 接管源对象的状态和资源租约。 */
  zero_copy_write(zero_copy_write&&) noexcept = default;

  /** @brief 禁止复制赋值。 */
  auto operator=(zero_copy_write&&) -> zero_copy_write& = delete;

  /** @brief 选择后端时也不在构造或 ready 阶段提交。 */
  bool await_ready() const noexcept { return false; }

  /** @brief 登记恢复目标，按等待协议交接挂起与完成责任。 */
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    auto* domain = resource_ ? resource_->owner.get() : nullptr;
    auto current = domain ? io::io_context{} : io::io_context::current();
    if (!domain && current)
      domain = current.domain().get();
    if (ip_stream_ && domain && domain->capabilities().zero_copy) {
      auto& operation =
          operation_.emplace<2>(resource_, buffer_.data(), buffer_.size(), no_signal_flags, 0);
      operation.empty_success();
      this->configure(operation);
      return operation.await_suspend(continuation);
    }
    auto& operation =
        operation_.emplace<1>(resource_, buffer_.data(), buffer_.size(), no_signal_flags);
    operation.empty_success();
    this->configure(operation);
    return operation.await_suspend(continuation);
  }

  /** @brief 完成交接后领取结果，并按接口约定传播失败或取消。 */
  auto await_resume() const noexcept -> expected<std::size_t> {
    if (const auto* operation = std::get_if<2>(&operation_))
      return operation->await_resume();
    if (const auto* operation = std::get_if<1>(&operation_))
      return operation->await_resume();
    return std::unexpected{make_error(EINVAL)};
  }

 private:
  io::detail::resource_ptr resource_; /**< lease 保持实际 domain 与句柄控制块有效。 */
  std::span<const char> buffer_;      /**< 借用 payload，调用者在最终结果返回前不得修改或销毁。 */
  bool ip_stream_{}; /**< 原生 SEND_ZC 的网络协议限制，不将 Unix socket 当作 TCP。 */
  std::variant<std::monostate, send_awaiter, send_zc_awaiter>
      operation_; /**< 仅所选请求建立操作状态。 */
};

/**  @brief TCP/Unix stream 写方向，默认不额外缓存数据，直接服从内核背压。 */
template <class T>
struct impl_stream_write {
  /** @brief 写入一段字节并返回本次实际写入数量。 */
  auto write(std::span<const char> buffer) const noexcept {
    auto operation = io::send(
        static_cast<const T*>(this)->resource(), buffer.data(), buffer.size(), no_signal_flags);
    operation.empty_success();
    return operation;
  }

  /** @brief 写入一段字节并返回本次实际写入数量。 */
  auto write(io::borrowed_const_buffer buffer) const noexcept { return write(buffer.bytes); }

  /**
   *  @brief 移入发送缓冲区，返回 buffer
   * 和本次实际进度，短写可继续发送剩余范围。
   */
  auto write(io::io_buffer buffer) const -> task<expected<io::io_transfer>> {
    return write_buffer_owned(static_cast<const T*>(this)->resource(), std::move(buffer));
  }

  /** @brief 写入一段字节并返回本次实际写入数量。 */
  auto write(io::shared_const_buffer buffer) const -> task<expected<std::size_t>> {
    return write_buffer_shared(static_cast<const T*>(this)->resource(), std::move(buffer));
  }

  /**
   *  @brief 优先采用 IP stream 原生零拷贝；能力不支持时安全降级普通发送。
   * @details 即使发送结果已产生，原生路径仍等待 buffer-release 通知后返回。
   */
  auto write_zc(std::span<const char> buffer) const noexcept {
    constexpr bool ip_stream = [] {
      if constexpr (requires { typename T::address_type; })
        return std::same_as<typename T::address_type, socket_addr>;
      else
        return false;
    }();
    return zero_copy_write{static_cast<const T*>(this)->resource(), buffer, ip_stream};
  }

  /** @brief 发起聚集写入，按实际完成字节数解释短写。 */
  auto write_vectored(std::span<const iovec> vectors) const noexcept {
    return borrowed_vectored_write{static_cast<const T*>(this)->resource(), vectors};
  }

  /** @brief 发起聚集写入，按实际完成字节数解释短写。 */
  template <class... Buffers>
    requires(sizeof...(Buffers) > 0 && (constructible_to_char_slice<Buffers> && ...))
  auto write_vectored(Buffers&&... buffers) const noexcept {
    return vectored_write<sizeof...(Buffers)>{static_cast<const T*>(this)->resource(),
                                              std::forward<Buffers>(buffers)...};
  }

  /** @brief 提交多个缓冲区的聚集写入。 */
  template <class... Buffers>
  auto write_v(Buffers&&... buffers) const noexcept {
    return write_vectored(std::forward<Buffers>(buffers)...);
  }

  /** @brief 即时尝试一次写入，不等待 socket 就绪。 */
  [[nodiscard]] auto try_write(std::span<const char> buffer) const -> expected<std::size_t> {
    auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::interest::WRITABLE, [&]() -> expected<std::size_t> {
          if (buffer.empty())
            return std::size_t{0};
          decltype(socket_send(0, nullptr, 0, 0)) result{};
          for (int attempt = 0; attempt < 8; ++attempt) {
            result = socket_send(object->fd(), buffer.data(), buffer.size(), no_signal_flags);
            if (result >= 0 || !socket_interrupted(socket_error()))
              break;
          }
          if (result < 0)
            return std::unexpected{socket_error()};
          return static_cast<std::size_t>(result);
        });
  }

  /** @brief 即时尝试聚集写入，返回实际完成字节数。 */
  [[nodiscard]] auto try_write_vectored(std::span<const iovec> vectors) const
      -> expected<std::size_t> {
    auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::interest::WRITABLE, [&]() -> expected<std::size_t> {
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

  /**  @brief 完整发送；短写继续推进，非空 buffer 的零进度返回 WriteZero。 */
  task<expected<void>> write_all(std::span<const char> buffer) const {
    return write_all_impl(static_cast<const T*>(this)->resource(), buffer);
  }

  /** @brief 接管负载缓冲区并在写入完成后返还所有权与进度。 */
  task<expected<std::size_t>> write_owned(std::vector<char> buffer) const {
    return write_vector_owned(static_cast<const T*>(this)->resource(), std::move(buffer));
  }

  /** @brief 写完拥有型缓冲区中的全部负载。 */
  task<expected<void>> write_all_buf(std::span<const char> buffer) const {
    return write_all(buffer);
  }

  /**  @brief 无用户态写缓存，单次 write 完成即提交到内核。 */
  task<expected<void>> flush() const { co_return expected<void>{}; }

 private:
  /**  @brief 调用时捕获资源，拥有型 vector 的存储覆盖整个延迟启动与发送过程。 */
  static task<expected<std::size_t>> write_vector_owned(
      std::shared_ptr<io::detail::resource_state> resource, std::vector<char> buffer) {
    auto operation = io::send(std::move(resource), buffer.data(), buffer.size(), no_signal_flags);
    operation.empty_success();
    co_return co_await operation;
  }

  /** @brief 在操作存续期间持有写入缓冲区。 */
  static task<expected<io::io_transfer>> write_buffer_owned(
      std::shared_ptr<io::detail::resource_state> resource, io::io_buffer buffer) {
    auto operation = io::send(std::move(resource), buffer.data(), buffer.size(), no_signal_flags);
    operation.empty_success();
    auto result = co_await operation;
    if (!result)
      co_return std::unexpected{result.error()};
    co_return io::io_transfer{std::move(buffer), *result};
  }

  /** @brief 在操作存续期间共享持有不可变写入负载。 */
  static task<expected<std::size_t>> write_buffer_shared(
      std::shared_ptr<io::detail::resource_state> resource, io::shared_const_buffer buffer) {
    auto operation =
        io::send(std::move(resource), buffer.bytes().data(), buffer.size(), no_signal_flags);
    operation.empty_success();
    co_return co_await operation;
  }

  /** @brief 在持有写方向身份期间循环完成全部字节写入。 */
  static task<expected<void>> write_all_impl(std::shared_ptr<io::detail::resource_state> resource,
                                             std::span<const char> buffer) {
    if (buffer.empty())
      co_return expected<void>{};
    char owner_identity{};

    if (!resource)
      co_return std::unexpected{make_error(EBADF)};
    io::detail::borrowed_direction_lease reservation{
        *resource, io::interest::WRITABLE, &owner_identity};
    std::size_t transferred{};
    bool first_operation = true;
    while (!buffer.empty()) {
      {
        auto operation = io::send(resource, buffer.data(), buffer.size(), no_signal_flags);
        if (first_operation) {
          operation.establish_reservation(&owner_identity);
          first_operation = false;
        } else {
          operation.reservation(&owner_identity);
        }
        operation.empty_success();

        auto result = co_await operation;

        if (!result)
          co_return std::unexpected{
              result.error().with_progress(transferred + result.error().progress())};
        if (*result == 0)
          co_return std::unexpected{
              ::faio::error{::faio::error::WRITE_ZERO, transferred, error_domain::FAIO}};
        transferred += *result;
        buffer = buffer.subspan(*result);
      }

      co_await this_coro::yield_if_needed();
    }
    co_return expected<void>{};
  }
};
}  // namespace faio::net::detail
#endif
