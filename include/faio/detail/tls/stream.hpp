/**
 * @file faio/detail/tls/stream.hpp
 * @brief 拥有型 TLS 流及读写半部，公开操作只借用共享连接状态。
 */
#pragma once
#include "faio/detail/io/base/buffer.hpp"
#include "faio/detail/io/stream/operation.hpp"
#include "faio/detail/tls/detail/state.hpp"
#include <stdexcept>
#include <utility>

namespace faio::tls {
/** @brief 握手后基于异步传输的 TLS 双向字节流。 */
template <detail::protocol_transport S>
class tls_stream;
/** @brief 共享 TLS 连接状态的独立读取端。 */
template <detail::protocol_transport S>
class tls_read_half;
/** @brief 共享 TLS 连接状态的独立写入端。 */
template <detail::protocol_transport S>
class tls_write_half;

namespace detail {
/** @brief 驱动握手并返回验证完成的 TLS 流。 */
template <class S>
auto tls_handshake(std::shared_ptr<tls_state<S>>, io::operation_options)
    -> task<expected<tls_stream<S>>>;
/** @brief 读取明文字节，维护读取租约与取消进度。 */
template <class S>
auto tls_read(std::shared_ptr<tls_state<S>>, std::span<char>, io::operation_options)
    -> task<expected<std::size_t>>;
/** @brief 读取明文并随结果归还拥有型缓冲区。 */
template <class S>
auto tls_read_buffer(std::shared_ptr<tls_state<S>>, io::io_buffer, io::operation_options)
    -> task<expected<io::io_transfer>>;
/** @brief 写入明文字节并维护加密传输进度。 */
template <class S>
auto tls_write(std::shared_ptr<tls_state<S>>, std::span<const char>, io::operation_options)
    -> task<expected<std::size_t>>;
/** @brief 写入有效缓冲字节并随结果归还缓冲区。 */
template <class S>
auto tls_write_buffer(std::shared_ptr<tls_state<S>>, io::io_buffer, io::operation_options)
    -> task<expected<io::io_transfer>>;
/** @brief 在写方向租约下刷新 TLS 与底层传输。 */
template <class S>
auto tls_flush(std::shared_ptr<tls_state<S>>, io::operation_options) -> task<expected<void>>;
/** @brief 在写方向租约下完成 TLS 写端关闭。 */
template <class S>
auto tls_shutdown(std::shared_ptr<tls_state<S>>, io::operation_options) -> task<expected<void>>;
/** @brief 在截止时间和停止约束下关闭 TLS 连接。 */
template <class S>
auto tls_close(std::shared_ptr<tls_state<S>>, io::operation_options) -> task<expected<void>>;

/** @brief 对共享连接状态请求立即终止。 */
template <class S>
void abort_state(const std::shared_ptr<tls_state<S>>& state) noexcept {
  if (state)
    state->request_abort(error{static_cast<int>(errc::CLOSED), 0, error_domain::TLS});
}
/** @brief 仅向成功完成握手的内部实现开放流的构造入口。 */
struct stream_access;
}  // namespace detail

/** @brief 共享 TLS 连接状态的独立读取端。 */
template <detail::protocol_transport S>
class tls_read_half {
 public:
  /** @brief 禁止构造尚未关联会话的 TLS 读半部。 */
  tls_read_half() = delete;
  /** @brief 禁止复制构造 TLS 读半部。 */
  tls_read_half(const tls_read_half&) = delete;
  /** @brief 禁止复制赋值 TLS 读半部。 */
  auto operator=(const tls_read_half&) -> tls_read_half& = delete;
  /** @brief 转移 TLS 读半部的会话所有权，源对象随后不再持有会话。 */
  tls_read_half(tls_read_half&&) noexcept = default;

  /** @brief 先终止当前会话，再接管源对象的 TLS 读半部。 */
  auto operator=(tls_read_half&& other) noexcept -> tls_read_half& {
    if (this != &other) {
      abort();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  /** @brief 释放 TLS 读半部的所有权，并请求终止整个共享会话。 */
  ~tls_read_half() noexcept { abort(); }

  /**
   * @brief 读取明文到借用缓冲；只在本次结果胜出后修改该缓冲。
   * @param bytes 返回前保持有效的可写明文缓冲。
   * @param options 显式取消令牌和绝对截止时间。
   * @return 实际交付字节数；收到对端 close_notify 且暂存明文已耗尽时为 0。
   */
  auto read(std::span<char> bytes, io::operation_options options = {}) const
      -> task<expected<std::size_t>> {
    return detail::tls_read(state_, bytes, std::move(options));
  }

  /**
   * @brief 移入拥有型缓冲并复用相同明文读取驱动。
   * @param buffer 随操作转移所有权的可写缓冲。
   * @param options 显式取消令牌和绝对截止时间。
   * @return 包含原缓冲和实际交付量的 io_transfer；缓冲 size 更新为实际读取量。
   */
  auto read(io::io_buffer buffer, io::operation_options options = {}) const
      -> task<expected<io::io_transfer>> {
    return detail::tls_read_buffer(state_, std::move(buffer), std::move(options));
  }

  /** @brief 非阻塞地终止整个共享会话，半部也适用。 */
  void abort() noexcept { detail::abort_state(state_); }

  /**
   * @brief 读取终止快照，不执行网络操作。
   * @return 已失败、已关闭或移动后返回 true。
   */
  auto is_terminated() const noexcept -> bool { return !state_ || state_->is_terminated(); }

  /**
   * @brief 复制协商结果和对端证书 DER，避免返回对 SSL 内部数据的借用。
   * @return 拥有型连接信息快照。
   */
  auto connection_info() const -> ::faio::tls::connection_info {
    if (!state_)
      throw std::logic_error("moved TLS half");
    return state_->connection_info();
  }

 private:
  /** @brief 允许完整流在 into_split 时构造共享会话的读写半部。 */
  friend class tls_stream<S>;

  /**
   * @brief 持有拆分会话的共享所有权，只公开读方向操作。
   * @param state 与另一半部或当前操作共享的连接状态。
   */
  explicit tls_read_half(std::shared_ptr<detail::tls_state<S>> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::tls_state<S>> state_; /**< 支撑异步操作和半部的共享会话所有权。 */
};

/** @brief 共享 TLS 连接状态的独立写入端。 */
template <detail::protocol_transport S>
class tls_write_half {
 public:
  /** @brief 禁止构造尚未关联会话的 TLS 写半部。 */
  tls_write_half() = delete;
  /** @brief 禁止复制构造 TLS 写半部。 */
  tls_write_half(const tls_write_half&) = delete;
  /** @brief 禁止复制赋值 TLS 写半部。 */
  auto operator=(const tls_write_half&) -> tls_write_half& = delete;
  /** @brief 转移 TLS 写半部的会话所有权，源对象随后不再持有会话。 */
  tls_write_half(tls_write_half&&) noexcept = default;

  /** @brief 先终止当前会话，再接管源对象的 TLS 写半部。 */
  auto operator=(tls_write_half&& other) noexcept -> tls_write_half& {
    if (this != &other) {
      abort();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  /** @brief 释放 TLS 写半部的所有权，并请求终止整个共享会话。 */
  ~tls_write_half() noexcept { abort(); }

  /**
   * @brief 接纳至多 16 KiB 明文，未接纳的尾部由调用方继续提交。
   * @param bytes 返回前保持有效的只读明文视图。
   * @param options 显式取消令牌和绝对截止时间。
   * @return SSL 实际接纳的明文字节数；成功并不表示密文已经排空。
   */
  auto write(std::span<const char> bytes, io::operation_options options = {}) const
      -> task<expected<std::size_t>> {
    return detail::tls_write(state_, bytes, std::move(options));
  }

  /**
   * @brief 移入拥有型明文缓冲并复用相同写入驱动。
   * @param buffer 随操作转移所有权的明文缓冲。
   * @param options 显式取消令牌和绝对截止时间。
   * @return 归还原缓冲及实际接纳的明文量，不把密文长度当作写入量。
   */
  auto write(io::io_buffer buffer, io::operation_options options = {}) const
      -> task<expected<io::io_transfer>> {
    return detail::tls_write_buffer(state_, std::move(buffer), std::move(options));
  }

  /**
   * @brief 排空已接纳明文产生的密文，并等待底层 flush。
   * @param options 取消令牌和绝对截止时间。
   * @return 成功或错误；失败进度不暴露密文字节数。
   */
  auto flush(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::tls_flush(state_, std::move(options));
  }

  /**
   * @brief 发送本方向 close_notify，之后仍允许读取对端。
   * @param options 取消令牌和绝对截止时间。
   * @return 本地通知已排空，或错误。
   */
  auto shutdown(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::tls_shutdown(state_, std::move(options));
  }

  /**
   * @brief 协调双向 close_notify 和底层关闭，默认总期限为 5 秒。
   * @param options 取消令牌和可覆盖的绝对截止时间。
   * @return 共享的关闭结果；未读应用数据不会静默丢弃。
   */
  auto close(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::tls_close(
        state_, io::detail::with_default_deadline(std::move(options), std::chrono::seconds(5)));
  }

  /** @brief 非阻塞地终止整个共享会话，半部也适用。 */
  void abort() noexcept { detail::abort_state(state_); }

  /**
   * @brief 读取终止快照，不执行网络操作。
   * @return 已失败、已关闭或移动后返回 true。
   */
  auto is_terminated() const noexcept -> bool { return !state_ || state_->is_terminated(); }

  /**
   * @brief 复制协商结果和对端证书 DER，避免返回对 SSL 内部数据的借用。
   * @return 拥有型连接信息快照。
   */
  auto connection_info() const -> ::faio::tls::connection_info {
    if (!state_)
      throw std::logic_error("moved TLS half");
    return state_->connection_info();
  }

 private:
  /** @brief 允许完整流在 into_split 时构造共享会话的读写半部。 */
  friend class tls_stream<S>;

  /**
   * @brief 持有拆分会话的共享所有权，只公开写方向和关闭操作。
   * @param state 与另一半部或当前操作共享的连接状态。
   */
  explicit tls_write_half(std::shared_ptr<detail::tls_state<S>> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::tls_state<S>> state_; /**< 支撑异步操作和半部的共享会话所有权。 */
};

/** @brief 握手后基于异步传输的 TLS 双向字节流。 */
template <detail::protocol_transport S>
class tls_stream {
 public:
  /** @brief 同一 TLS 会话的只读半部类型。 */
  using read_half = tls_read_half<S>;
  /** @brief 同一 TLS 会话的只写半部类型。 */
  using write_half = tls_write_half<S>;
  /** @brief 禁止构造尚未关联会话的 TLS 流。 */
  tls_stream() = delete;
  /** @brief 禁止复制构造 TLS 流。 */
  tls_stream(const tls_stream&) = delete;
  /** @brief 禁止复制赋值 TLS 流。 */
  auto operator=(const tls_stream&) -> tls_stream& = delete;
  /** @brief 转移 TLS 流的会话所有权，源对象随后不再持有会话。 */
  tls_stream(tls_stream&&) noexcept = default;

  /** @brief 先终止当前会话，再接管源对象的 TLS 流。 */
  auto operator=(tls_stream&& other) noexcept -> tls_stream& {
    if (this != &other) {
      abort();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  /** @brief 释放 TLS 流的所有权，并请求终止整个共享会话。 */
  ~tls_stream() noexcept { abort(); }

  /**
   * @brief 读取明文到借用缓冲；只在本次结果胜出后修改该缓冲。
   * @param bytes 返回前保持有效的可写明文缓冲。
   * @param options 显式取消令牌和绝对截止时间。
   * @return 实际交付字节数；收到对端 close_notify 且暂存明文已耗尽时为 0。
   */
  auto read(std::span<char> bytes, io::operation_options options = {}) const
      -> task<expected<std::size_t>> {
    return detail::tls_read(state_, bytes, std::move(options));
  }

  /**
   * @brief 移入拥有型缓冲并复用相同明文读取驱动。
   * @param buffer 随操作转移所有权的可写缓冲。
   * @param options 显式取消令牌和绝对截止时间。
   * @return 包含原缓冲和实际交付量的 io_transfer；缓冲 size 更新为实际读取量。
   */
  auto read(io::io_buffer buffer, io::operation_options options = {}) const
      -> task<expected<io::io_transfer>> {
    return detail::tls_read_buffer(state_, std::move(buffer), std::move(options));
  }

  /**
   * @brief 接纳至多 16 KiB 明文，未接纳的尾部由调用方继续提交。
   * @param bytes 返回前保持有效的只读明文视图。
   * @param options 显式取消令牌和绝对截止时间。
   * @return SSL 实际接纳的明文字节数；成功并不表示密文已经排空。
   */
  auto write(std::span<const char> bytes, io::operation_options options = {}) const
      -> task<expected<std::size_t>> {
    return detail::tls_write(state_, bytes, std::move(options));
  }

  /**
   * @brief 移入拥有型明文缓冲并复用相同写入驱动。
   * @param buffer 随操作转移所有权的明文缓冲。
   * @param options 显式取消令牌和绝对截止时间。
   * @return 归还原缓冲及实际接纳的明文量，不把密文长度当作写入量。
   */
  auto write(io::io_buffer buffer, io::operation_options options = {}) const
      -> task<expected<io::io_transfer>> {
    return detail::tls_write_buffer(state_, std::move(buffer), std::move(options));
  }

  /**
   * @brief 排空已接纳明文产生的密文，并等待底层 flush。
   * @param options 取消令牌和绝对截止时间。
   * @return 成功或错误；失败进度不暴露密文字节数。
   */
  auto flush(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::tls_flush(state_, std::move(options));
  }

  /**
   * @brief 发送本方向 close_notify，之后仍允许读取对端。
   * @param options 取消令牌和绝对截止时间。
   * @return 本地通知已排空，或错误。
   */
  auto shutdown(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::tls_shutdown(state_, std::move(options));
  }

  /**
   * @brief 协调双向 close_notify 和底层关闭，默认总期限为 5 秒。
   * @param options 取消令牌和可覆盖的绝对截止时间。
   * @return 共享的关闭结果；未读应用数据不会静默丢弃。
   */
  auto close(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::tls_close(
        state_, io::detail::with_default_deadline(std::move(options), std::chrono::seconds(5)));
  }

  /** @brief 非阻塞地终止整个共享会话，半部也适用。 */
  void abort() noexcept { detail::abort_state(state_); }

  /**
   * @brief 读取终止快照，不执行网络操作。
   * @return 已失败、已关闭或移动后返回 true。
   */
  auto is_terminated() const noexcept -> bool { return !state_ || state_->is_terminated(); }

  /**
   * @brief 复制协商结果和对端证书 DER，避免返回对 SSL 内部数据的借用。
   * @return 拥有型连接信息快照。
   */
  auto connection_info() const -> ::faio::tls::connection_info {
    if (!state_)
      throw std::logic_error("moved TLS stream");
    return state_->connection_info();
  }

  /**
   * @brief 将没有活动应用操作的流拆成共享会话的只读和只写半部。
   * @return 共同拥有原连接状态的两个半部，原流随后失去所有权。
   * @throws std::logic_error 对象已移动或读/写方向仍存在活动操作。
   */
  auto into_split() && -> std::pair<read_half, write_half> {
    if (!state_)
      throw std::logic_error("moved TLS stream");
    std::lock_guard lock(state_->mutex);
    if (state_->read_gate.load(std::memory_order_acquire)
        || state_->write_gate.load(std::memory_order_acquire)
        || (state_->closing && !state_->close_result))
      throw std::logic_error("cannot split TLS stream with active operations");
    auto state = std::move(state_);
    return {read_half{state}, write_half{std::move(state)}};
  }

 private:
  /** @brief 仅允许内部握手完成路径调用私有流构造器。 */
  friend struct detail::stream_access;

  /**
   * @brief 保存握手驱动交付的共享会话，不重新初始化 SSL。
   * @param state 与另一半部或当前操作共享的连接状态。
   */
  explicit tls_stream(std::shared_ptr<detail::tls_state<S>> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::tls_state<S>> state_; /**< 支撑异步操作和半部的共享会话所有权。 */
};

namespace detail {
/** @brief 仅向成功完成握手的内部实现开放流的构造入口。 */
struct stream_access {
  /** @brief 从共享连接状态构造 TLS 流。 */
  template <protocol_transport S>
  static auto stream(std::shared_ptr<tls_state<S>> state) -> tls_stream<S> {
    return tls_stream<S>{std::move(state)};
  }
};
}  // namespace detail
}  // namespace faio::tls

#include "faio/detail/tls/detail/drive.hpp"
