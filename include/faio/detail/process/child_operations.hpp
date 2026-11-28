/**
 * @file faio/detail/process/child_operations.hpp
 * @brief 等待、终止及公共控制器的退出门协调。
 */
#pragma once
#include "faio/detail/process/exit_wait_operation.hpp"

namespace faio::process::detail {
/** @brief 串行协调退出门、可选 stdin 关闭和终态等待。 */
inline auto wait_child_impl(std::shared_ptr<process_state> state,
                            std::shared_ptr<pipe_state> input,
                            io::operation_options options,
                            bool terminate) -> task<expected<exit_status>> {
  if (!state)
    co_return std::unexpected{make_error(EBADF)};
  auto parent = co_await this_coro::stop_token();
  if (auto checked = preflight(options, parent); !checked)
    co_return std::unexpected{checked.error()};
  auto gate = state->try_acquire_exit_gate();
  if (!gate)
    co_return std::unexpected{gate.error()};

  struct gate_guard {
    std::shared_ptr<process_state> state; /**< 保证退出门所属记录存活至守卫归还。 */

    /** @brief 归还退出门，使取消和异常返回都不会阻塞后续 wait。 */
    ~gate_guard() { state->release_exit_gate(); }
  } guard{state};

  if (state->runtime_stopped())
    co_return std::unexpected{process_error(errc::RUNTIME_SHUTDOWN)};
  if (terminate) {
    auto killed = request_termination(state);
    if (!killed)
      co_return std::unexpected{killed.error()};
  }
  if (input) {
    auto closed = co_await input->close(options);
    if (!closed)
      co_return std::unexpected{closed.error()};
  }
  auto operation = exit_wait_operation::create(state, options, parent);
  if (!operation)
    co_return std::unexpected{operation.error()};
  co_return co_await (*operation)->wait();
}

/** @brief 在已移出标准流的内部路径中异步等待进程退出。 */
inline auto wait_impl(std::shared_ptr<process_state> state, io::operation_options options)
    -> task<expected<exit_status>> {
  return wait_child_impl(std::move(state), {}, options, false);
}

/** @brief 在内部路径中请求终止并等待退出确认。 */
inline auto kill_impl(std::shared_ptr<process_state> state, io::operation_options options)
    -> task<expected<void>> {
  auto result = co_await wait_child_impl(std::move(state), {}, options, true);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}

/** @brief 先请求终止，再关闭仍由 Child 持有的 stdin 并确认退出。 */
inline auto public_kill(std::shared_ptr<process_state> state,
                        std::shared_ptr<pipe_state> input,
                        io::operation_options options) -> task<expected<void>> {
  auto result = co_await wait_child_impl(std::move(state), std::move(input), options, true);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}
}  // namespace faio::process::detail

namespace faio::process {
/** @brief 返回尚未确认退出的进程 ID；无控制权或终态时为空。 */
inline auto child::id() const noexcept -> std::optional<process_id> {
  return state_ ? state_->id() : std::nullopt;
}

/** @brief 关闭仍由 Child 持有的 stdin，再通过独占退出门等待终态。 */
inline auto child::wait(io::operation_options options) -> task<expected<exit_status>> {
  return detail::wait_child_impl(state_, input_ ? input_->state_ : nullptr, options, false);
}

/** @brief 请求强制终止并异步等待退出确认。 */
inline auto child::kill(io::operation_options options) -> task<expected<void>> {
  return detail::public_kill(state_, input_ ? input_->state_ : nullptr, options);
}
#if defined(_WIN32)
/** @brief 借用仍受进程记录管理的 Windows 进程句柄。 */
inline auto child::raw_handle() const noexcept -> void* {
  return state_ ? state_->process_ : nullptr;
}
#endif
}  // namespace faio::process
