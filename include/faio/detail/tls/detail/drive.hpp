/**
 * @file faio/detail/tls/detail/drive.hpp
 * @brief 借助调用方协程推进 TLS 握手、读写、取消和安全关闭。
 */
#pragma once
#include "faio/detail/io/stream/operation.hpp"
#include "faio/detail/tls/stream.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <stop_token>

namespace faio::tls::detail {

/** @brief 把一个停止通知转发到组合停止源。 */
struct forward_request {
  std::stop_source* target; /**< 停止通知要转发到的目标源。 */

  /** @brief 向组合停止源转发停止请求。 */
  void operator()() const noexcept { target->request_stop(); }
};

/** @brief 把操作取消、连接终止及本地关闭合并为同一个停止令牌。 */
struct combined_stop {
  std::stop_source source; /**< 供内部驱动使用的组合停止源。 */
  std::optional<std::stop_callback<forward_request>> first{}, second{},
      third{}; /**< 本地关闭通知的可选转发注册。 */

  /**
   * @brief 注册各停止源的转发回调，任意源触发都会停止内部驱动。
   * @param a 当前操作继承的停止令牌。
   * @param b 连接终止令牌。
   * @param c 可选的本地读关闭令牌。
   */
  combined_stop(std::stop_token a, std::stop_token b, std::stop_token c = {}) {
    if (a.stop_possible())
      first.emplace(a, forward_request{&source});
    if (b.stop_possible())
      second.emplace(b, forward_request{&source});
    if (c.stop_possible())
      third.emplace(c, forward_request{&source});
  }
};

/**
 * @brief 区分取消或超时与真正的传输失败；取消或超时应保留连接状态记录。
 * @param error 底层操作错误。
 * @return POSIX 取消或超时返回 true。
 */
inline bool interrupted(const error& error) noexcept {
  return error.domain() == error_domain::POSIX
         && (error.value() == ECANCELED || error.value() == ETIMEDOUT);
}

/** @brief 向组合操作提交 TLS 结果，由终态仲裁决定是否接纳。 */
template <class T>
void commit_result(io::detail::stream_operation_state<T>& operation, expected<T> result) {
  operation.try_commit([&]() noexcept -> expected<T> { return std::move(result); });
}

/** @brief 绑定停止令牌驱动 TLS，并转换连接终止原因。 */
template <class S, class T>
auto drive_bound(std::shared_ptr<tls_state<S>> state,
                 io::detail::stream_operation_state<T>& operation,
                 task<void> drive,
                 std::stop_token token) -> task<void> {
  try {
    co_await io::detail::bind_stop(std::move(drive), std::move(token));
  } catch (const operation_cancelled&) {
    std::optional<error> failure;
    {
      std::lock_guard lock(state->mutex);
      failure = state->failure;
    }
    if (!failure)
      throw;
    commit_result(operation, expected<T>{std::unexpected{failure->with_progress(0)}});
  }
}

/** @brief 读取一次密文并更新 TLS 输入状态。 */
template <class S>
auto wire_read_step(std::shared_ptr<tls_state<S>> state) -> task<void> {
  const auto observed = state->changed.snapshot();
  auto lease = gate_lease::try_acquire(state->wire_read_gate, &state->changed);
  if (!lease) {
    co_await state->changed.wait_change(observed);
    co_return;
  }
  std::size_t capacity{};
  {
    std::lock_guard lock(state->mutex);
    if (state->failure || state->closed || state->transport_eof)
      co_return;
    capacity = std::min<std::size_t>(16 * 1024, state->engine.input_capacity());
  }
  if (!capacity) {
    co_await state->changed.wait_change(observed);
    co_return;
  }
  std::array<char, 16 * 1024> input{};
  auto result = co_await state->transport.read(std::span<char>{input}.first(capacity));
  std::optional<error> failure;
  {
    std::lock_guard lock(state->mutex);
    const auto bytes =
        result ? *result : std::min<std::uint64_t>(result.error().progress(), capacity);
    if (bytes) {
      auto fed = state->engine.feed_ciphertext(std::span<const char>{input}.first(bytes));
      if (!fed)
        failure = fed.error();
      else if (*fed != bytes)
        failure = error{static_cast<int>(errc::PROTOCOL_ERROR), 0, error_domain::TLS};
    }
    if (result && *result == 0) {
      state->transport_eof = true;
      state->engine.notify_transport_eof();
    }
    if (!result && !interrupted(result.error()))
      failure = result.error();
  }
  if (failure)
    state->request_abort(*failure);
  state->changed.notify_all();
}

/** @brief 发送待写密文并更新 TLS 输出进度。 */
template <class S>
auto wire_write_step(std::shared_ptr<tls_state<S>> state) -> task<void> {
  const auto observed = state->changed.snapshot();
  auto lease = gate_lease::try_acquire(state->wire_write_gate, &state->changed);
  if (!lease) {
    co_await state->changed.wait_change(observed);
    co_return;
  }
  std::span<const char> output;
  std::optional<error> failure;
  {
    std::lock_guard lock(state->mutex);
    if (state->failure || state->closed)
      co_return;
    if (state->wire_offset == state->wire_size) {
      state->wire_offset = state->wire_size = 0;
      auto bytes = state->engine.take_ciphertext(state->wire_output);
      if (!bytes)
        failure = bytes.error();
      else
        state->wire_size = *bytes;
    }
    output = std::span<const char>{state->wire_output}.subspan(
        state->wire_offset, state->wire_size - state->wire_offset);
  }
  if (failure) {
    state->request_abort(*failure);
    co_return;
  }
  if (output.empty()) {
    co_await state->changed.wait_change(observed);
    co_return;
  }
  auto result = co_await state->transport.write(output);
  {
    std::lock_guard lock(state->mutex);
    const auto bytes =
        result ? *result : std::min<std::uint64_t>(result.error().progress(), output.size());
    if (bytes > output.size())
      failure = error{static_cast<int>(errc::PROTOCOL_ERROR), 0, error_domain::TLS};
    else
      state->wire_offset += bytes;
    if (result && !*result)
      failure = error{error::WRITE_ZERO, 0, error_domain::FAIO};
    if (!result && !interrupted(result.error()))
      failure = result.error();
  }
  if (failure)
    state->request_abort(*failure);
  state->changed.notify_all();
}

/** @brief 推进所需的传输方向或等待连接状态变化。 */
template <class S>
auto network_progress(std::shared_ptr<tls_state<S>> state, bool read) -> task<void> {
  auto token = co_await this_coro::stop_token();
  if (token.stop_requested())
    throw operation_cancelled{};
  const auto observed = state->changed.snapshot();
  if (read)
    co_await select(
        wire_read_step(state), wire_write_step(state), state->changed.wait_change(observed));
  else
    co_await select(wire_write_step(state), state->changed.wait_change(observed));
  co_await this_coro::yield_if_needed();
}

/** @brief 发送全部待写密文并刷新底层传输。 */
template <class S>
auto flush_output(std::shared_ptr<tls_state<S>> state) -> task<expected<void>> {
  while (true) {
    bool pending;
    {
      std::lock_guard lock(state->mutex);
      if (state->failure)
        co_return std::unexpected{*state->failure};
      pending = state->wire_offset != state->wire_size || state->engine.output_pending() != 0;
    }
    if (!pending)
      break;
    co_await network_progress(state, false);
  }
  co_return co_await detail::transport_flush(state->transport);
}

/** @brief 驱动握手并返回验证完成的 TLS 流。 */
template <class S>
auto tls_handshake(std::shared_ptr<tls_state<S>> state, io::operation_options options)
    -> task<expected<tls_stream<S>>> {
  if (!state)
    co_return std::unexpected{error{EBADF}};
  auto result = co_await io::detail::run_operation<void>(
      options, [state](io::detail::stream_operation_state<void>& operation) -> task<void> {
        auto parent = co_await this_coro::stop_token();
        combined_stop stop{parent, state->abort_stop.get_token()};
        auto drive = [state, &operation]() -> task<void> {
          while (!operation.done()) {
            expected<step_result> step = std::unexpected{error{EIO}};
            {
              std::lock_guard lock(state->mutex);
              step = state->failure ? expected<step_result>{std::unexpected{*state->failure}}
                                    : state->engine.handshake_step();
            }
            if (!step) {
              commit_result(operation, expected<void>{std::unexpected{step.error()}});
              co_return;
            }
            if (step->status == step_status::COMPLETE) {
              auto flushed = co_await flush_output(state);
              commit_result(operation, std::move(flushed));
              co_return;
            }
            co_await network_progress(state, step->status == step_status::WANT_READ);
          }
        };
        co_await drive_bound(state, operation, drive(), stop.source.get_token());
      });
  if (!result) {
    state->request_abort(result.error());
    static_cast<void>(co_await io::detail::bind_stop(detail::transport_close(state->transport),
                                                     std::stop_token{}));
    co_return std::unexpected{result.error().with_progress(0)};
  }
  co_return stream_access::stream(std::move(state));
}

/** @brief 读取明文字节，维护读取租约与取消进度。 */
template <class S>
auto tls_read(std::shared_ptr<tls_state<S>> state,
              std::span<char> bytes,
              io::operation_options options) -> task<expected<std::size_t>> {
  if (!state)
    co_return std::unexpected{error{EBADF}};
  auto lease = state->try_enter(app_direction::READ);
  if (!lease)
    co_return std::unexpected{lease.error()};
  auto result = co_await io::detail::run_operation<std::size_t>(
      options,
      [state, bytes](io::detail::stream_operation_state<std::size_t>& operation) -> task<void> {
        auto parent = co_await this_coro::stop_token();
        combined_stop stop{parent, state->abort_stop.get_token(), state->read_close.get_token()};
        auto drive = [state, bytes, &operation]() -> task<void> {
          if (bytes.empty()) {
            operation.try_commit([&]() noexcept -> expected<std::size_t> {
              std::lock_guard lock(state->mutex);
              if (state->failure)
                return std::unexpected{*state->failure};
              if (state->closing)
                return std::unexpected{error{ECANCELED}};
              return std::size_t{};
            });
            co_return;
          }
          while (!operation.done()) {
            step_status status = step_status::WANT_READ;
            std::optional<error> failure;
            bool ready = false;
            {
              std::lock_guard lock(state->mutex);
              if (state->failure)
                failure = state->failure;
              else if (state->plain_offset < state->plain_size || state->peer_shutdown)
                ready = true;
              else {
                auto step = state->engine.read_step(state->plain_read);
                if (!step)
                  failure = step.error();
                else {
                  status = step->status;
                  if (status == step_status::COMPLETE) {
                    state->plain_offset = 0;
                    state->plain_size = step->plaintext_bytes;
                    ready = true;
                  }
                  if (status == step_status::PEER_CLOSED) {
                    state->peer_shutdown = true;
                    ready = true;
                  }
                }
              }
            }
            state->changed.notify_all();
            if (failure) {
              state->request_abort(*failure);
              commit_result(operation, expected<std::size_t>{std::unexpected{*failure}});
              co_return;
            }
            if (ready) {
              operation.try_commit([&]() noexcept -> expected<std::size_t> {
                std::lock_guard lock(state->mutex);
                if (state->failure)
                  return std::unexpected{*state->failure};
                if (state->closing)
                  return std::unexpected{error{ECANCELED}};
                const auto count = std::min(bytes.size(), state->plain_size - state->plain_offset);
                if (count)
                  std::memcpy(bytes.data(), state->plain_read.data() + state->plain_offset, count);
                state->plain_offset += count;
                return count;
              });
              co_return;
            }
            co_await network_progress(state, status == step_status::WANT_READ);
          }
        };
        co_await drive_bound(state, operation, drive(), stop.source.get_token());
      });
  if (!result)
    co_return std::unexpected{result.error().with_progress(0)};
  co_return result;
}

/** @brief 读取明文并随结果归还拥有型缓冲区。 */
template <class S>
auto tls_read_buffer(std::shared_ptr<tls_state<S>> state,
                     io::io_buffer buffer,
                     io::operation_options options) -> task<expected<io::io_transfer>> {
  auto result = co_await tls_read(std::move(state), buffer.writable_bytes(), std::move(options));
  if (!result)
    co_return std::unexpected{result.error()};
  buffer.set_size(*result);
  co_return io::io_transfer{std::move(buffer), *result};
}

/** @brief 写入明文字节并维护加密传输进度。 */
template <class S>
auto tls_write(std::shared_ptr<tls_state<S>> state,
               std::span<const char> bytes,
               io::operation_options options) -> task<expected<std::size_t>> {
  if (!state)
    co_return std::unexpected{error{EBADF}};
  auto lease = state->try_enter(app_direction::WRITE);
  if (!lease)
    co_return std::unexpected{lease.error()};
  {
    std::lock_guard lock(state->mutex);
    if (state->local_shutdown || state->peer_shutdown)
      co_return std::unexpected{error{static_cast<int>(errc::CLOSED), 0, error_domain::TLS}};
  }
  bool dirty = false;
  std::size_t accepted{};
  auto result = co_await io::detail::run_operation<std::size_t>(
      options,
      [state, bytes, &dirty, &accepted](
          io::detail::stream_operation_state<std::size_t>& operation) -> task<void> {
        auto parent = co_await this_coro::stop_token();
        combined_stop stop{parent, state->abort_stop.get_token()};
        auto drive = [state, bytes, &dirty, &accepted, &operation]() -> task<void> {
          if (bytes.empty()) {
            operation.try_commit([&]() noexcept -> expected<std::size_t> {
              std::lock_guard lock(state->mutex);
              if (state->failure)
                return std::unexpected{*state->failure};
              if (state->closing)
                return std::unexpected{error{ECANCELED}};
              return std::size_t{};
            });
            co_return;
          }
          {
            std::lock_guard lock(state->mutex);
            state->write_size = std::min(bytes.size(), state->plain_write.size());
            std::memcpy(state->plain_write.data(), bytes.data(), state->write_size);
          }
          while (!operation.done()) {
            step_status status = step_status::WANT_WRITE;
            std::optional<error> failure;
            bool retry_application = false;
            {
              std::lock_guard lock(state->mutex);
              if (state->failure)
                failure = state->failure;
              else {
                const auto pending = state->engine.pending_call();
                if (pending && pending->call == pending_call::READ
                    && pending->last_status == step_status::WANT_WRITE) {
                  auto step = state->engine.read_step(state->plain_read);
                  if (!step)
                    failure = step.error();
                  else {
                    status = step->status;
                    if (status == step_status::COMPLETE) {
                      state->plain_size = step->plaintext_bytes;
                      state->plain_offset = 0;
                    }
                    retry_application =
                        status == step_status::COMPLETE || status == step_status::WANT_READ;
                    if (status == step_status::PEER_CLOSED) {
                      state->peer_shutdown = true;
                      failure = error{static_cast<int>(errc::CLOSED), 0, error_domain::TLS};
                    }
                  }
                } else {
                  dirty = true;
                  auto step = state->engine.write_step(
                      std::span<const char>{state->plain_write}.first(state->write_size));
                  if (!step)
                    failure = step.error();
                  else {
                    status = step->status;
                    if (status == step_status::COMPLETE)
                      accepted = step->plaintext_bytes;
                  }
                }
              }
            }
            state->changed.notify_all();
            if (failure) {
              state->request_abort(*failure);
              commit_result(
                  operation,
                  expected<std::size_t>{std::unexpected{failure->with_progress(accepted)}});
              co_return;
            }
            if (accepted) {
              commit_result(operation, expected<std::size_t>{accepted});
              co_return;
            }
            if (retry_application) {
              co_await this_coro::yield_if_needed();
              continue;
            }
            co_await network_progress(state, status == step_status::WANT_READ);
          }
        };
        co_await drive_bound(state, operation, drive(), stop.source.get_token());
      });
  if (!result && dirty)
    state->request_abort(result.error().with_progress(accepted));
  if (!result)
    co_return std::unexpected{result.error().with_progress(accepted)};
  co_return result;
}

/** @brief 写入有效缓冲字节并随结果归还缓冲区。 */
template <class S>
auto tls_write_buffer(std::shared_ptr<tls_state<S>> state,
                      io::io_buffer buffer,
                      io::operation_options options) -> task<expected<io::io_transfer>> {
  auto result = co_await tls_write(std::move(state), buffer.bytes(), std::move(options));
  if (!result)
    co_return std::unexpected{result.error()};
  co_return io::io_transfer{std::move(buffer), *result};
}

/** @brief 在写方向租约下刷新 TLS 与底层传输。 */
template <class S>
auto tls_flush(std::shared_ptr<tls_state<S>> state, io::operation_options options)
    -> task<expected<void>> {
  if (!state)
    co_return std::unexpected{error{EBADF}};
  auto lease = state->try_enter(app_direction::WRITE);
  if (!lease)
    co_return std::unexpected{lease.error()};
  bool started = false;
  auto result = co_await io::detail::run_operation<void>(
      options,
      [state, &started](io::detail::stream_operation_state<void>& operation) -> task<void> {
        auto parent = co_await this_coro::stop_token();
        combined_stop stop{parent, state->abort_stop.get_token()};
        started = true;
        auto drive = [state, &operation]() -> task<void> {
          auto flushed = co_await flush_output(state);
          commit_result(operation, std::move(flushed));
        };
        co_await drive_bound(state, operation, drive(), stop.source.get_token());
      });
  if (!result && started)
    state->request_abort(result.error());
  if (!result)
    co_return std::unexpected{result.error().with_progress(0)};
  co_return result;
}

/** @brief 发送 close_notify 并刷新本地 TLS 输出。 */
template <class S>
auto shutdown_output(std::shared_ptr<tls_state<S>> state) -> task<expected<void>> {
  auto flushed = co_await flush_output(state);
  if (!flushed)
    co_return flushed;
  while (true) {
    expected<step_result> step = std::unexpected{error{EIO}};
    bool finishing_read = false;
    {
      std::lock_guard lock(state->mutex);
      if (state->failure)
        co_return std::unexpected{*state->failure};
      if (state->local_shutdown)
        co_return expected<void>{};
      const auto pending = state->engine.pending_call();
      finishing_read = pending && pending->call == pending_call::READ
                       && pending->last_status == step_status::WANT_WRITE;
      if (finishing_read) {
        step = state->engine.read_step(state->plain_read);
        if (step && step->status == step_status::COMPLETE) {
          state->plain_offset = 0;
          state->plain_size = step->plaintext_bytes;
        }
        if (step && step->status == step_status::PEER_CLOSED)
          state->peer_shutdown = true;
      } else {
        step = state->engine.shutdown_step();
        if (step
            && (step->status == step_status::COMPLETE
                || step->status == step_status::LOCAL_SHUTDOWN))
          state->local_shutdown = true;
        if (step && step->status == step_status::COMPLETE)
          state->peer_shutdown = true;
      }
    }
    state->changed.notify_all();
    if (!step)
      co_return std::unexpected{step.error()};
    if (finishing_read && step->status != step_status::WANT_WRITE) {
      co_await this_coro::yield_if_needed();
      continue;
    }
    if (!finishing_read
        && (step->status == step_status::COMPLETE || step->status == step_status::LOCAL_SHUTDOWN))
      co_return co_await flush_output(state);
    co_await network_progress(state, step->status == step_status::WANT_READ);
  }
}

/** @brief 在写方向租约下完成 TLS 写端关闭。 */
template <class S>
auto tls_shutdown(std::shared_ptr<tls_state<S>> state, io::operation_options options)
    -> task<expected<void>> {
  if (!state)
    co_return std::unexpected{error{EBADF}};
  {
    std::lock_guard lock(state->mutex);
    if (state->local_shutdown && !state->failure)
      co_return expected<void>{};
  }
  auto lease = state->try_enter(app_direction::WRITE);
  if (!lease)
    co_return std::unexpected{lease.error()};
  bool started = false;
  auto result = co_await io::detail::run_operation<void>(
      options,
      [state, &started](io::detail::stream_operation_state<void>& operation) -> task<void> {
        auto parent = co_await this_coro::stop_token();
        combined_stop stop{parent, state->abort_stop.get_token()};
        started = true;
        auto drive = [state, &operation]() -> task<void> {
          auto shutdown = co_await shutdown_output(state);
          commit_result(operation, std::move(shutdown));
        };
        co_await drive_bound(state, operation, drive(), stop.source.get_token());
      });
  if (!result && started)
    state->request_abort(result.error());
  if (!result)
    co_return std::unexpected{result.error().with_progress(0)};
  co_return result;
}

/** @brief 协调活动操作退出并完成 TLS 连接关闭。 */
template <class S>
auto close_connection(std::shared_ptr<tls_state<S>> state) -> task<expected<void>> {
  while (true) {
    const auto observed = state->changed.snapshot();
    if (!state->read_gate.load(std::memory_order_acquire)
        && !state->write_gate.load(std::memory_order_acquire))
      break;
    co_await state->changed.wait_change(observed);
  }
  auto shutdown = co_await shutdown_output(state);
  if (!shutdown)
    co_return shutdown;
  while (true) {
    expected<step_result> step = std::unexpected{error{EIO}};
    {
      std::lock_guard lock(state->mutex);
      if (state->failure)
        co_return std::unexpected{*state->failure};
      if (state->plain_offset < state->plain_size)
        co_return std::unexpected{
            tls_error(errc::UNREAD_APPLICATION_DATA, "Unread application data during TLS close")};
      if (state->peer_shutdown)
        break;
      step = state->engine.read_step(state->plain_read);
      if (step && step->status == step_status::COMPLETE && step->plaintext_bytes)
        co_return std::unexpected{
            tls_error(errc::UNREAD_APPLICATION_DATA, "Unread application data during TLS close")};
      if (step && step->status == step_status::PEER_CLOSED)
        state->peer_shutdown = true;
    }
    if (!step)
      co_return std::unexpected{step.error()};
    if (step->status == step_status::PEER_CLOSED)
      break;
    co_await network_progress(state, step->status == step_status::WANT_READ);
  }
  auto flushed = co_await flush_output(state);
  if (!flushed)
    co_return flushed;
  co_return co_await detail::transport_close(state->transport);
}

/** @brief 在截止时间和停止约束下关闭 TLS 连接。 */
template <class S>
auto tls_close(std::shared_ptr<tls_state<S>> state, io::operation_options options)
    -> task<expected<void>> {
  if (!state)
    co_return std::unexpected{error{EBADF}};
  const auto parent = co_await this_coro::stop_token();
  if (parent.stop_requested() || options.stop.stop_requested())
    co_return std::unexpected{error{ECANCELED}};
  if (options.deadline && std::chrono::steady_clock::now() >= *options.deadline)
    co_return std::unexpected{error{ETIMEDOUT}};
  const bool owner = state->begin_close();
  auto result = co_await io::detail::run_operation<void>(
      options, [state, owner](io::detail::stream_operation_state<void>& operation) -> task<void> {
        if (!owner) {
          while (!operation.done()) {
            const auto observed = state->changed.snapshot();
            std::optional<expected<void>> result;
            {
              std::lock_guard lock(state->mutex);
              if (state->close_result)
                result = *state->close_result;
            }
            if (result) {
              commit_result(operation, std::move(*result));
              co_return;
            }
            co_await state->changed.wait_change(observed);
          }
          co_return;
        }
        auto parent = co_await this_coro::stop_token();
        combined_stop stop{parent, state->abort_stop.get_token()};
        auto drive = [state, &operation]() -> task<void> {
          auto closed = co_await close_connection(state);
          commit_result(operation, std::move(closed));
        };
        co_await drive_bound(state, operation, drive(), stop.source.get_token());
      });
  if (!owner && !result)
    state->request_abort(result.error());
  if (owner) {
    if (!result) {
      state->request_abort(result.error());
      static_cast<void>(co_await io::detail::bind_stop(detail::transport_close(state->transport),
                                                       std::stop_token{}));
    }
    {
      std::lock_guard lock(state->mutex);
      state->close_result = result;
      if (result) {
        state->closed = true;
        state->terminal.store(true, std::memory_order_release);
      }
    }
    state->changed.notify_all();
  }
  if (!result)
    co_return std::unexpected{result.error().with_progress(0)};
  co_return result;
}
}  // namespace faio::tls::detail
