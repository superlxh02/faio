/**
 * @file faio/detail/net/base/halves.hpp
 * @brief 读写半部与重新合并。
 */
#ifndef FAIO_DETAIL_NET_BASE_HALVES_HPP
#define FAIO_DETAIL_NET_BASE_HALVES_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/io/awaiter/ready.hpp"
#include "faio/detail/net/base/address.hpp"
#include "faio/detail/net/base/socket.hpp"
#include "faio/detail/net/base/stream_read.hpp"
#include "faio/detail/net/base/stream_write.hpp"
#include <memory>
#include <optional>

namespace faio::net::detail {
template <class Stream, class Addr, bool Owned>
class BasicWriteHalf;

/**  @brief 安全读半部：共享控制块，使父 stream 移动或析构后不会留下悬空指针。 */
template <class Stream, class Addr, bool Owned>
class BasicReadHalf : public ImplStreamRead<BasicReadHalf<Stream, Addr, Owned>>,
                      public ImplLocalAddr<BasicReadHalf<Stream, Addr, Owned>, Addr>,
                      public ImplPeerAddr<BasicReadHalf<Stream, Addr, Owned>, Addr> {
 public:
  /** @brief 保存读半部的连接租约和拆分身份。 */
  BasicReadHalf(Socket socket, std::shared_ptr<char> identity)
      : socket_{std::move(socket)}, identity_{std::move(identity)} {}

  /** @brief 接管源对象的状态和资源租约。 */
  BasicReadHalf(BasicReadHalf&&) noexcept = default;

  /** @brief 移动赋值资源及状态。 */
  auto operator=(BasicReadHalf&&) noexcept -> BasicReadHalf& = default;

  /** @brief 禁止复制此对象。 */
  BasicReadHalf(const BasicReadHalf&) = delete;

  /** @brief 禁止复制赋值。 */
  auto operator=(const BasicReadHalf&) -> BasicReadHalf& = delete;

  /** @brief 释放读半部的资源和拆分身份租约。 */
  ~BasicReadHalf() = default;

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto fd() const noexcept -> native_socket_type {
    return socket_ ? socket_->fd() : -1;
  }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto as_native_handle() const noexcept -> native_socket_type { return fd(); }

  /** @brief 取得底层共享资源控制块，保持句柄身份及所属域有效。 */
  [[nodiscard]] auto resource() const noexcept { return socket_ ? socket_->resource() : nullptr; }

  /** @brief 取得该对象关联的 I/O 上下文租约。 */
  [[nodiscard]] auto context() const noexcept {
    return socket_ ? socket_->context() : io::io_context{};
  }

  /** @brief 等待给定的 socket 就绪条件。 */
  auto ready(io::Interest interest = io::Interest::readable) const noexcept {
    return io::ready(resource(), interest);
  }

  /** @brief 等待 socket 或管道可读，不消费数据。 */
  auto readable() const noexcept { return ready(); }

  /** @brief 异步关闭共享套接字，等待在途操作安全结束。 */
  auto close() noexcept { return io::close(resource()); }

  /**  @brief 仅同一次 owned split 的半部可重新合并，需比较资源身份与 split 身份。 */
  [[nodiscard]] auto reunite(BasicWriteHalf<Stream, Addr, Owned>&& writer) && -> expected<Stream>
    requires Owned
  {
    if (!socket_ || !writer.socket_ || identity_ != writer.identity_
        || resource() != writer.resource())
      return std::unexpected{make_error(Error::ReuniteFailed)};
    Socket socket = socket_->share();
    writer.automatic_shutdown_ = false;
    writer.socket_.reset();
    writer.identity_.reset();
    socket_.reset();
    identity_.reset();
    return Stream{std::move(socket)};
  }

 private:
  friend class BasicWriteHalf<Stream, Addr, Owned>;
  std::optional<Socket>
      socket_; /**< 持有或共享底层 socket 包装，在挂起期间保持原生资源身份有效。 */
  std::shared_ptr<char> identity_; /**< 读写半部共享的身份租约，合并时验证来自同一原始流。 */
};

/**  @brief 独立写半部；owned 半部析构在在途写排空后请求写半关闭。 */
template <class Stream, class Addr, bool Owned>
class BasicWriteHalf : public ImplStreamWrite<BasicWriteHalf<Stream, Addr, Owned>>,
                       public ImplLocalAddr<BasicWriteHalf<Stream, Addr, Owned>, Addr>,
                       public ImplPeerAddr<BasicWriteHalf<Stream, Addr, Owned>, Addr> {
 public:
  using address_type = Addr; /**< 半部保留原 stream 地址协议与零拷贝能力限制。 */

  /** @brief 保存写半部的连接租约和拆分身份。 */
  BasicWriteHalf(Socket socket, std::shared_ptr<char> identity)
      : socket_{std::move(socket)}, identity_{std::move(identity)} {}

  /** @brief 接管源对象的状态和资源租约。 */
  BasicWriteHalf(BasicWriteHalf&& other) noexcept
      : socket_{std::move(other.socket_)},
        identity_{std::move(other.identity_)},
        automatic_shutdown_{std::exchange(other.automatic_shutdown_, false)} {
    other.socket_.reset();
  }

  /** @brief 移动赋值资源及状态。 */
  auto operator=(BasicWriteHalf&& other) noexcept -> BasicWriteHalf& {
    if (this != &other) {
      request_shutdown();
      socket_ = std::move(other.socket_);
      identity_ = std::move(other.identity_);
      automatic_shutdown_ = std::exchange(other.automatic_shutdown_, false);
      other.socket_.reset();
    }
    return *this;
  }

  /** @brief 禁止复制此对象。 */
  BasicWriteHalf(const BasicWriteHalf&) = delete;

  /** @brief 禁止复制赋值。 */
  auto operator=(const BasicWriteHalf&) -> BasicWriteHalf& = delete;

  /** @brief 按拥有型写半部策略请求写半关闭并释放租约。 */
  ~BasicWriteHalf() { request_shutdown(); }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto fd() const noexcept -> native_socket_type {
    return socket_ ? socket_->fd() : -1;
  }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto as_native_handle() const noexcept -> native_socket_type { return fd(); }

  /** @brief 取得底层共享资源控制块，保持句柄身份及所属域有效。 */
  [[nodiscard]] auto resource() const noexcept { return socket_ ? socket_->resource() : nullptr; }

  /** @brief 取得该对象关联的 I/O 上下文租约。 */
  [[nodiscard]] auto context() const noexcept {
    return socket_ ? socket_->context() : io::io_context{};
  }

  /** @brief 等待给定的 socket 就绪条件。 */
  auto ready(io::Interest interest = io::Interest::writable) const noexcept {
    return io::ready(resource(), interest);
  }

  /** @brief 等待 socket 或管道可写，不执行实际写入。 */
  auto writable() const noexcept { return ready(); }

  /** @brief 请求关闭 socket 的指定传输方向。 */
  auto shutdown() noexcept { return io::shutdown(resource(), SHUT_WR); }

  /** @brief 异步关闭共享套接字，等待在途操作安全结束。 */
  auto close() noexcept { return io::close(resource()); }

  /**  @brief 析构时不再关闭写半部，资源仍由其他拥有者保持有效。 */
  void forget() noexcept
    requires Owned
  {
    automatic_shutdown_ = false;
  }

  /** @brief 合并同一次拆分产生的拥有型读写半部。 */
  [[nodiscard]] auto reunite(BasicReadHalf<Stream, Addr, Owned>&& reader) && -> expected<Stream>
    requires Owned
  {
    return std::move(reader).reunite(std::move(*this));
  }

 private:
  friend class BasicReadHalf<Stream, Addr, Owned>;

  /** @brief 按拥有型写半部的析构策略请求写半关闭。 */
  void request_shutdown() noexcept {
    if constexpr (Owned) {
      if (automatic_shutdown_ && socket_ && socket_->resource())
        socket_->resource()->request_shutdown_write();
    }
    automatic_shutdown_ = false;
  }

  std::optional<Socket>
      socket_; /**< 持有或共享底层 socket 包装，在挂起期间保持原生资源身份有效。 */
  std::shared_ptr<char> identity_; /**< 读写半部共享的身份租约，合并时验证来自同一原始流。 */
  bool automatic_shutdown_{Owned}; /**< 析构拥有型写半部时是否自动请求写半关闭。 */
};
}  // namespace faio::net::detail
#endif
