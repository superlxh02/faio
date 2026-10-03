#ifndef FAIO_DETAIL_NET_TCP_HALVES_HPP
#define FAIO_DETAIL_NET_TCP_HALVES_HPP
#include "faio/detail/net/common/platform.hpp"
#include "faio/detail/net/common/addr_util.hpp"
#include "faio/detail/net/common/socket.hpp"
#include "faio/detail/net/common/stream_read.hpp"
#include "faio/detail/net/common/stream_write.hpp"
#include <memory>
#include <optional>

namespace faio::net::detail {
template <class Stream, class Addr, bool Owned>
class BasicWriteHalf;

/** @brief 安全读半边：控制块共享保证父 stream 移动/析构不会留下悬空指针。 */
template <class Stream, class Addr, bool Owned>
class BasicReadHalf : public ImplStreamRead<BasicReadHalf<Stream, Addr, Owned>>,
                      public ImplLocalAddr<BasicReadHalf<Stream, Addr, Owned>, Addr>,
                      public ImplPeerAddr<BasicReadHalf<Stream, Addr, Owned>, Addr> {
 public:
  BasicReadHalf(Socket socket, std::shared_ptr<char> identity)
      : socket_{std::move(socket)}, identity_{std::move(identity)} {}

  BasicReadHalf(BasicReadHalf&&) noexcept = default;

  auto operator=(BasicReadHalf&&) noexcept -> BasicReadHalf& = default;

  BasicReadHalf(const BasicReadHalf&) = delete;

  auto operator=(const BasicReadHalf&) -> BasicReadHalf& = delete;

  ~BasicReadHalf() = default;

  [[nodiscard]] auto fd() const noexcept -> native_socket_type {
    return socket_ ? socket_->fd() : -1;
  }

  [[nodiscard]] auto as_native_handle() const noexcept -> native_socket_type { return fd(); }

  [[nodiscard]] auto resource() const noexcept { return socket_ ? socket_->resource() : nullptr; }

  [[nodiscard]] auto context() const noexcept {
    return socket_ ? socket_->context() : io::io_context{};
  }

  auto ready(io::Interest interest = io::Interest::readable) const noexcept {
    return io::ready(resource(), interest);
  }

  auto readable() const noexcept { return ready(); }

  auto close() noexcept { return io::close(resource()); }

  /** @brief 仅同一次 owned split 的半边可重合，比较资源身份与 split 身份。 */
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
  std::optional<Socket> socket_;
  std::shared_ptr<char> identity_;
};

/** @brief 独立写半边；owned 半边析构在在途写排空后请求写半关闭。 */
template <class Stream, class Addr, bool Owned>
class BasicWriteHalf : public ImplStreamWrite<BasicWriteHalf<Stream, Addr, Owned>>,
                       public ImplLocalAddr<BasicWriteHalf<Stream, Addr, Owned>, Addr>,
                       public ImplPeerAddr<BasicWriteHalf<Stream, Addr, Owned>, Addr> {
 public:
  using address_type = Addr;  ///< 半边保留原 stream 地址协议与零拷贝能力限制。

  BasicWriteHalf(Socket socket, std::shared_ptr<char> identity)
      : socket_{std::move(socket)}, identity_{std::move(identity)} {}

  BasicWriteHalf(BasicWriteHalf&& other) noexcept
      : socket_{std::move(other.socket_)},
        identity_{std::move(other.identity_)},
        automatic_shutdown_{std::exchange(other.automatic_shutdown_, false)} {
    other.socket_.reset();
  }

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

  BasicWriteHalf(const BasicWriteHalf&) = delete;

  auto operator=(const BasicWriteHalf&) -> BasicWriteHalf& = delete;

  ~BasicWriteHalf() { request_shutdown(); }

  [[nodiscard]] auto fd() const noexcept -> native_socket_type {
    return socket_ ? socket_->fd() : -1;
  }

  [[nodiscard]] auto as_native_handle() const noexcept -> native_socket_type { return fd(); }

  [[nodiscard]] auto resource() const noexcept { return socket_ ? socket_->resource() : nullptr; }

  [[nodiscard]] auto context() const noexcept {
    return socket_ ? socket_->context() : io::io_context{};
  }

  auto ready(io::Interest interest = io::Interest::writable) const noexcept {
    return io::ready(resource(), interest);
  }

  auto writable() const noexcept { return ready(); }

  auto shutdown() noexcept { return io::shutdown(resource(), SHUT_WR); }

  auto close() noexcept { return io::close(resource()); }

  /** @brief 放弃析构写半关闭，资源仍由其他拥有者安全保持。 */
  void forget() noexcept
    requires Owned
  {
    automatic_shutdown_ = false;
  }

  [[nodiscard]] auto reunite(BasicReadHalf<Stream, Addr, Owned>&& reader) && -> expected<Stream>
    requires Owned
  {
    return std::move(reader).reunite(std::move(*this));
  }

 private:
  friend class BasicReadHalf<Stream, Addr, Owned>;

  void request_shutdown() noexcept {
    if constexpr (Owned) {
      if (automatic_shutdown_ && socket_ && socket_->resource())
        socket_->resource()->request_shutdown_write();
    }
    automatic_shutdown_ = false;
  }

  std::optional<Socket> socket_;
  std::shared_ptr<char> identity_;
  bool automatic_shutdown_{Owned};
};
}  // namespace faio::net::detail
#endif
