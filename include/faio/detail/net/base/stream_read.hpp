/**
 * @file faio/detail/net/base/stream_read.hpp
 * @brief 网络地址、socket 与传输协议：字节流读取算法。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#ifndef FAIO_DETAIL_NET_BASE_STREAM_READ_HPP
#define FAIO_DETAIL_NET_BASE_STREAM_READ_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/base/buffer.hpp"
#include "faio/detail/io/io.hpp"
#include "faio/detail/net/base/awaiter_options.hpp"
#include <array>
#include <climits>
#include <span>
#include <vector>

namespace faio::net::detail {
/** @brief recvmsg 组合 awaiter；iovec 与 msghdr 始终位于最终协程帧内。 */
template <std::size_t N>
class VectoredRead : public AwaiterOptions<VectoredRead<N>> {
 public:
  template <class... Buffers>
  VectoredRead(std::shared_ptr<io::detail::resource_state> resource, Buffers&&... buffers)
      : resource_{std::move(resource)},
        vectors_{iovec{std::span<char>{buffers}.data(), std::span<char>{buffers}.size_bytes()}...},
        message_{[&] {
          msghdr message{};
          message.msg_iov = vectors_.data();
          message.msg_iovlen = N;
          return message;
        }()},
        operation_{io::recvmsg(resource_, &message_, 0)} {
    /// @brief 成员原位配置，避免 && builder 按值移动整份请求。
    operation_.empty_success();
    message_.msg_iov = vectors_.data();
    message_.msg_iovlen = N;
  }

  VectoredRead(const VectoredRead&) = delete;

  /** @brief 仅提交前可移动；重建操作使原生消息指针指向新 awaiter。 */
  VectoredRead(VectoredRead&& other) noexcept
      : AwaiterOptions<VectoredRead<N>>{std::move(other)},
        resource_{std::move(other.resource_)},
        vectors_{other.vectors_},
        message_{[&] {
          msghdr message{};
          message.msg_iov = vectors_.data();
          message.msg_iovlen = N;
          return message;
        }()},
        operation_{io::recvmsg(resource_, &message_, 0)} {
    /// @brief 成员原位配置，避免 && builder 按值移动整份请求。
    operation_.empty_success();
    message_.msg_iov = vectors_.data();
    message_.msg_iovlen = N;
  }

  /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
  auto await_ready() noexcept { return operation_.await_ready(); }

  /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
  auto await_suspend(std::coroutine_handle<> continuation) {
    this->configure(operation_);
    return operation_.await_suspend(continuation);
  }

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  auto await_resume() noexcept { return operation_.await_resume(); }

 private:
  std::shared_ptr<io::detail::resource_state> resource_;  ///< 共享资源控制块租约，跨挂起固定句柄身份及 IO 域归属。
  std::array<iovec, N> vectors_{};  ///< 分散聚集 IO 的缓冲区描述，负载仍须由调用方保持有效。
  msghdr message_{};  ///< 稳定保存的消息描述及缓冲区引用，安全完成前不得迁移。
  decltype(io::recvmsg(
      std::declval<std::shared_ptr<io::detail::resource_state>>(), nullptr, 0)) operation_;  ///< 地址稳定的底层异步操作，当前包装负责其启动、等待与结果交付。
};

/** @brief 借用 iovec 数组的分散读取，不复制 payload、不新增 task 帧。 */
class BorrowedVectoredRead : public AwaiterOptions<BorrowedVectoredRead> {
 public:
  BorrowedVectoredRead(std::shared_ptr<io::detail::resource_state> resource,
                       std::span<iovec> vectors)
      : resource_{std::move(resource)},
        message_{[&] {
          msghdr message{};
          message.msg_iov = vectors.data();
          message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(vectors.size());
          return message;
        }()},
        operation_{io::recvmsg(resource_, &message_, 0)} {
    /// @brief 成员原位配置，避免 && builder 按值移动整份请求。
    operation_.empty_success();
    message_.msg_iov = vectors.data();
    message_.msg_iovlen = static_cast<decltype(msghdr{}.msg_iovlen)>(vectors.size());
  }

  BorrowedVectoredRead(const BorrowedVectoredRead&) = delete;

  BorrowedVectoredRead(BorrowedVectoredRead&& other) noexcept
      : AwaiterOptions<BorrowedVectoredRead>{std::move(other)},
        resource_{std::move(other.resource_)},
        message_{other.message_},
        operation_{io::recvmsg(resource_, &message_, 0)} {
    /// @brief 成员原位配置，避免 && builder 按值移动整份请求。
    operation_.empty_success();
  }

  /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
  auto await_ready() noexcept { return operation_.await_ready(); }

  /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
  auto await_suspend(std::coroutine_handle<> continuation) {
    this->configure(operation_);
    return operation_.await_suspend(continuation);
  }

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  auto await_resume() noexcept { return operation_.await_resume(); }

 private:
  std::shared_ptr<io::detail::resource_state> resource_;  ///< 共享资源控制块租约，跨挂起固定句柄身份及 IO 域归属。
  msghdr message_{};  ///< 稳定保存的消息描述及缓冲区引用，安全完成前不得迁移。
  decltype(io::recvmsg(
      std::declval<std::shared_ptr<io::detail::resource_state>>(), nullptr, 0)) operation_;  ///< 地址稳定的底层异步操作，当前包装负责其启动、等待与结果交付。
};

/** @brief TCP/Unix stream 读方向接口，空 buffer 成功但不表示 EOF。 */
template <class T>
struct ImplStreamRead {
  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  auto read(std::span<char> buffer) const noexcept {
    auto operation =
        io::recv(static_cast<const T*>(this)->resource(), buffer.data(), buffer.size(), 0);
    /// @brief 左值配置不移动大型 io_request，return 保持 NRVO。
    operation.empty_success();
    return operation;
  }

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  auto read(io::borrowed_buffer buffer) const noexcept { return read(buffer.bytes); }

  /** @brief 独占缓冲区随协程帧保存，完成后返还实际初始化范围。 */
  auto read(io::io_buffer buffer) const -> task<expected<io::io_transfer>> {
    return read_buffer_owned(static_cast<const T*>(this)->resource(), std::move(buffer));
  }

  auto peek(std::span<char> buffer) const noexcept {
    auto operation =
        io::recv(static_cast<const T*>(this)->resource(), buffer.data(), buffer.size(), MSG_PEEK);
    operation.empty_success();
    return operation;
  }

  /// @brief 发起分散读取，缓冲区描述及负载在完成前均须有效。
  auto read_vectored(std::span<iovec> vectors) const noexcept {
    return BorrowedVectoredRead{static_cast<const T*>(this)->resource(), vectors};
  }

  /// @brief 发起分散读取，缓冲区描述及负载在完成前均须有效。
  template <class... Buffers>
    requires(sizeof...(Buffers) > 0
             && (requires(Buffers& buffer) { std::span<char>{buffer}; } && ...))
  auto read_vectored(Buffers&&... buffers) const noexcept {
    return VectoredRead<sizeof...(Buffers)>{static_cast<const T*>(this)->resource(),
                                            std::forward<Buffers>(buffers)...};
  }

  template <class... Buffers>
  auto read_v(Buffers&&... buffers) const noexcept {
    return read_vectored(std::forward<Buffers>(buffers)...);
  }

  /** @brief 同步尝试读取；would_block 返回 EAGAIN，不覆盖现有异步 waiter。 */
  [[nodiscard]] auto try_read(std::span<char> buffer) const -> expected<std::size_t> {
    return try_read_flags(buffer, 0);
  }

  [[nodiscard]] auto try_peek(std::span<char> buffer) const -> expected<std::size_t> {
    return try_read_flags(buffer, MSG_PEEK);
  }

  [[nodiscard]] auto try_read_vectored(std::span<iovec> vectors) const -> expected<std::size_t> {
    auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::Interest::readable, [&]() -> expected<std::size_t> {
          if (vectors.size() > IOV_MAX)
            return std::unexpected{make_error(EINVAL)};
          const auto result =
              socket_readv(object->fd(), vectors.data(), static_cast<int>(vectors.size()));
          if (result < 0)
            return std::unexpected{socket_error()};
          return static_cast<std::size_t>(result);
        });
  }

  /** @brief 精确读满；中途 EOF 返回 UnexpectedEOF，已经消费的数据保留在
   * buffer。 */
  task<expected<void>> read_exact(std::span<char> buffer) const {
    return read_exact_impl(static_cast<const T*>(this)->resource(), buffer);
  }

  task<expected<void>> read_bytes(std::span<char> buffer) const { return read_exact(buffer); }

  /** @brief 读取到拥有型 buffer，返回实际读取的初始化字节。 */
  task<expected<std::vector<char>>> read_owned(std::size_t capacity) const {
    return read_vector_owned(static_cast<const T*>(this)->resource(), capacity);
  }

  /** @brief 向 vector 尾部追加一次读取，所有提交范围先初始化。 */
  task<expected<std::size_t>> read_buf(std::vector<char>& buffer, std::size_t count = 8192) const {
    return read_vector_append(static_cast<const T*>(this)->resource(), buffer, count);
  }

  auto read_buf(io::read_buf& buffer) const -> task<expected<std::size_t>> {
    return read_initialized(static_cast<const T*>(this)->resource(), buffer);
  }

  [[nodiscard]] auto try_read_buf(io::read_buf& buffer) const -> expected<std::size_t> {
    auto result = try_read(buffer.unfilled());
    if (result)
      buffer.advance(*result);
    return result;
  }

  [[nodiscard]] auto try_read_buf(std::vector<char>& buffer, std::size_t count = 8192) const
      -> expected<std::size_t> {
    const auto original = buffer.size();
    if (count > buffer.max_size() - original)
      return std::unexpected{make_error(EOVERFLOW)};
    buffer.resize(original + count);
    auto result = try_read(std::span<char>{buffer}.subspan(original));
    buffer.resize(original + (result ? *result : 0));
    return result;
  }

 private:
  /** @brief 非成员协程拥有 resource 快照；首次恢复前移动包装对象也不会解引用旧
   * this。 */
  static task<expected<std::vector<char>>> read_vector_owned(
      std::shared_ptr<io::detail::resource_state> resource, std::size_t capacity) {
    std::vector<char> buffer(capacity);
    auto operation = io::recv(std::move(resource), buffer.data(), buffer.size(), 0);
    operation.empty_success();
    /// @brief 借用帧内稳定 awaiter，避免配置链逐次移动请求。
    auto result = co_await operation;
    if (!result)
      co_return std::unexpected{result.error()};
    buffer.resize(*result);
    co_return buffer;
  }

  static task<expected<std::size_t>> read_vector_append(
      std::shared_ptr<io::detail::resource_state> resource,
      std::vector<char>& buffer,
      std::size_t count) {
    const auto original = buffer.size();
    if (count > buffer.max_size() - original)
      co_return std::unexpected{make_error(EOVERFLOW)};
    buffer.resize(original + count);
    const auto writable = std::span<char>{buffer}.subspan(original);
    auto operation = io::recv(std::move(resource), writable.data(), writable.size(), 0);
    operation.empty_success();
    auto result = co_await operation;
    buffer.resize(original + (result ? *result : 0));
    co_return result;
  }

  static task<expected<std::size_t>> read_initialized(
      std::shared_ptr<io::detail::resource_state> resource, io::read_buf& buffer) {
    const auto writable = buffer.unfilled();
    auto operation = io::recv(std::move(resource), writable.data(), writable.size(), 0);
    operation.empty_success();
    auto result = co_await operation;
    if (result)
      buffer.advance(*result);
    co_return result;
  }

  static task<expected<io::io_transfer>> read_buffer_owned(
      std::shared_ptr<io::detail::resource_state> resource, io::io_buffer buffer) {
    auto operation = io::recv(std::move(resource), buffer.data(), buffer.capacity(), 0);
    operation.empty_success();
    auto result = co_await operation;
    if (!result)
      co_return std::unexpected{result.error()};
    buffer.set_size(*result);
    co_return io::io_transfer{std::move(buffer), *result};
  }

  static task<expected<void>> read_exact_impl(std::shared_ptr<io::detail::resource_state> resource,
                                              std::span<char> buffer) {
    if (buffer.empty())
      co_return expected<void>{};
    /// @brief 地址位于稳定组合协程帧，仅作为方向 reservation 身份。
    char owner_identity{};

    auto reservation =
        io::detail::reserve_direction(resource, io::Interest::readable, &owner_identity);
    if (!reservation)
      co_return std::unexpected{reservation.error()};
    std::size_t transferred{};
    while (!buffer.empty()) {
      {
        auto operation = io::recv(resource, buffer.data(), buffer.size(), 0);
        operation.reservation(&owner_identity);
        operation.empty_success();
        /// @brief await_transform 只借用稳定局部对象，不构造移动链。
        auto result = co_await operation;

        if (!result)
          co_return std::unexpected{Error{result.error().value(),
                                          transferred + result.error().progress(),
                                          result.error().domain()}};
        if (*result == 0)
          co_return std::unexpected{Error{Error::UnexpectedEOF, transferred, error_domain::faio}};
        transferred += *result;
        buffer = buffer.subspan(*result);
      /// @brief 已完成 awaiter/stop callback 在协作让出前退出，组合方向租约仍继续持有。
      }

      co_await this_coro::yield_if_needed();
    }
    co_return expected<void>{};
  }

  [[nodiscard]] auto try_read_flags(std::span<char> buffer, int flags) const
      -> expected<std::size_t> {
    auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::Interest::readable, [&]() -> expected<std::size_t> {
          if (buffer.empty())
            return std::size_t{0};
          decltype(socket_recv(0, nullptr, 0, 0)) result{};
          /// @brief EINTR 有界重试；不会在 worker 上因信号风暴永久占用 CPU。
          for (int attempt = 0; attempt < 8; ++attempt) {
            result = socket_recv(object->fd(), buffer.data(), buffer.size(), flags);
            if (result >= 0 || !socket_interrupted(socket_error()))
              break;
          }
          if (result < 0)
            return std::unexpected{socket_error()};
          return static_cast<std::size_t>(result);
        });
  }
};
}
#endif
