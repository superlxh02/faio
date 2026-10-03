#pragma once

#include "faio/detail/coroutine/task_context.hpp"
#include "faio/detail/execution/blocking_executor.hpp"
#include "faio/detail/io/context.hpp"
#include <atomic>
#include <chrono>
#include <concepts>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <variant>

namespace faio::execution {
namespace detail {
template <class T> struct unwrap_expected {
  using type = T;
  static constexpr bool wrapped = false;
};
template <class T> struct unwrap_expected<expected<T>> {
  using type = T;
  static constexpr bool wrapped = true;
};

/** @brief 任务稳定状态；池、等待者和 stop_callback 共享同一完成位置。 */
template <class F> struct execute_state {
  using raw_result = std::invoke_result_t<F &>;
  using result_type = typename unwrap_expected<raw_result>::type;
  static_assert(!std::is_reference_v<result_type>, "阻塞服务不能返回借用引用");
  io::io_context context;         // 保持所属 IO 控制块和服务租约存活。
  blocking_executor_ref executor; // 按请求选择文件、DNS或清理服务。
  bool cancellable{true};         // close等清理操作必须执行，不被父令牌跳过。
  F function; // 提交前拥有完整系统调用参数，不借用临时 lambda。
  std::optional<expected<result_type>> result; // 完成仲裁后写一次。
  scheduler_ref scheduler;                     // 唤醒原任务的中立调度接口。
  std::atomic<unsigned char> handshake{0};     // 0 提交中，1 已挂起，2 已完成。
  std::mutex mutex; // 只仲裁取消/结果，不跨系统调用持锁。
  std::optional<int>
      cancellation_reason; ///< 第一个停止原因固定，后来的deadline不覆盖取消。
  bool terminal{};         // 结果发布以后，迟到取消不再修改结果。
  std::optional<std::chrono::steady_clock::time_point> deadline;
  struct cancel_callback {
    execute_state *state;
    void operator()() const noexcept {
      std::lock_guard lock(state->mutex);
      if (!state->terminal && !state->cancellation_reason)
        state->cancellation_reason = state->timed_out() ? ETIMEDOUT : ECANCELED;
    }
  };
  std::optional<std::stop_callback<cancel_callback>> stop_callback;
  std::optional<std::stop_callback<cancel_callback>> domain_stop_callback;
  execute_state(io::io_context ctx, F operation, blocking_executor_ref service,
                bool can_cancel)
      : context(std::move(ctx)), executor(std::move(service)),
        cancellable(can_cancel), function(std::move(operation)) {}
  bool timed_out() const noexcept {
    return cancellable && deadline &&
           std::chrono::steady_clock::now() >= *deadline;
  }
  /** @brief 在mutex保护下记录截止时间，保持先到的停止原因。 */
  void observe_deadline() noexcept {
    if (!cancellation_reason && timed_out())
      cancellation_reason = ETIMEDOUT;
  }
  /** @brief 完成/取消在同一短锁中决定终态，恢复只经过保存的调度器。 */
  void complete(expected<result_type> value) noexcept {
    {
      std::lock_guard lock(mutex);
      // 被取消时保留已执行的字节进度，绝不在 syscall 排空前恢复借用者。
      observe_deadline();
      if (cancellation_reason) {
        std::size_t progress = 0;
        if (!value)
          progress = value.error().progress();
        else if constexpr (std::integral<result_type>)
          progress = static_cast<std::size_t>(*value);
        value = std::unexpected{Error{*cancellation_reason, progress}};
      }
      result.emplace(std::move(value));
      terminal = true;
    }
    // 先 release 结果，再通过握手判断是否需要异步调度等待者。
    if (handshake.exchange(2, std::memory_order_acq_rel) == 1)
      scheduler.schedule(continuation);
  }
  std::coroutine_handle<> continuation;
  void run() noexcept {
    {
      std::lock_guard lock(mutex);
      // 排队期间停止可跳过 syscall；状态仍由已接受 job 完成。
      observe_deadline();
      if (cancellation_reason) {
        result.emplace(std::unexpected{make_error(*cancellation_reason)});
        terminal = true;
      }
    }
    if (terminal) {
      if (handshake.exchange(2, std::memory_order_acq_rel) == 1)
        scheduler.schedule(continuation);
      return;
    }
    try {
      if constexpr (unwrap_expected<raw_result>::wrapped)
        complete(std::invoke(function));
      else if constexpr (std::is_void_v<raw_result>) {
        std::invoke(function);
        complete(expected<void>{});
      } else
        complete(expected<result_type>{std::invoke(function)});
    } catch (const std::filesystem::filesystem_error &error) {
      complete(std::unexpected{make_error(error.code().value())});
    } catch (const std::bad_alloc &) {
      complete(std::unexpected{make_error(ENOMEM)});
    } catch (...) {
      complete(std::unexpected{make_error(EIO)});
    }
  }
};
} // namespace detail

/** @brief 类型化阻塞请求的协程桥接，构造时不提交、不执行系统调用。 */
template <class F> class execute_awaiter {
public:
  using state_type = detail::execute_state<F>;
  using result_type = typename state_type::result_type;
  execute_awaiter(io::io_context context, F function,
                  blocking_executor_ref executor, bool cancellable = true)
      : state_(std::make_shared<state_type>(std::move(context),
                                            std::move(function),
                                            std::move(executor), cancellable)) {
  }
  execute_awaiter(const execute_awaiter &) = delete;
  execute_awaiter &operator=(const execute_awaiter &) = delete;
  execute_awaiter(execute_awaiter &&) = default;
  /** @brief 设置相对超时；借用型操作等待已开始 syscall 排空后才返回。 */
  template <class Rep, class Period>
  execute_awaiter &set_timeout(std::chrono::duration<Rep, Period> duration) & {
    state_->deadline = std::chrono::steady_clock::now() + duration;
    return *this;
  }
  template <class Rep, class Period>
  execute_awaiter &&
  set_timeout(std::chrono::duration<Rep, Period> duration) && {
    set_timeout(duration);
    return std::move(*this);
  }
  execute_awaiter &
  set_timeout_at(std::chrono::steady_clock::time_point deadline) & {
    state_->deadline = deadline;
    return *this;
  }
  execute_awaiter &&
  set_timeout_at(std::chrono::steady_clock::time_point deadline) && {
    set_timeout_at(deadline);
    return std::move(*this);
  }
  bool await_ready() const noexcept { return false; }
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    // 局部 lease 覆盖发布后立即在其他 worker 恢复和销毁 awaiter 的竞态。
    auto state = state_;
    state->continuation = continuation;
    std::stop_token token;
    if constexpr (requires { continuation.promise().context; }) {
      state->scheduler = continuation.promise().context.scheduler;
      token = continuation.promise().context.stop_token;
    } else {
      state->scheduler = ::faio::detail::current_scheduler();
      token = ::faio::detail::current_stop_token;
    }
    if (!state->scheduler)
      throw std::logic_error("阻塞 IO 必须由运行时调度");
    // callback 构造可同步触发；此时共享状态已经完整建立。
    if (state->cancellable) {
      if (token.stop_possible())
        state->stop_callback.emplace(
            token, typename state_type::cancel_callback{state.get()});
      auto domain_token = state->context.stop_token();
      if (domain_token.stop_possible())
        state->domain_stop_callback.emplace(
            domain_token, typename state_type::cancel_callback{state.get()});
    }
    // 服务容量不足直接完成，调用者得到明确背压错误。
    if (!state->cancellable) {
      // 清理不占普通admission额度；饱和时由domain保留完成责任并重试。
      state->context.defer_cleanup([state] { state->run(); });
    } else {
      auto accepted = state->executor.try_submit([state] { state->run(); });
      if (!accepted)
        state->complete(std::unexpected{accepted.error()});
    }
    unsigned char submitting = 0;
    // 先完成时返回 false，已挂起时由完成线程排队恢复，保证唯一唤醒。
    return state->handshake.compare_exchange_strong(submitting, 1,
                                                    std::memory_order_acq_rel);
  }
  expected<result_type> await_resume() {
    state_->stop_callback.reset();
    state_->domain_stop_callback.reset();
    return std::move(state_->result.value());
  }

private:
  std::shared_ptr<state_type> state_;
};

/** @brief 提交受控系统调用到 context 的文件服务，自动展开 expected 返回值。 */
template <class F>
  requires std::invocable<std::decay_t<F> &>
auto execute(io::io_context context, F &&function) {
  auto service = context.blocking();
  return execute_awaiter<std::decay_t<F>>{
      std::move(context), std::forward<F>(function), std::move(service)};
}
/** @brief 显式选择阻塞服务，例如 resolver lane；参数及完成状态均拥有生命周期。
 */
template <class F>
  requires std::invocable<std::decay_t<F> &>
auto execute_blocking(io::io_context context, F &&function,
                      blocking_executor_ref executor) {
  return execute_awaiter<std::decay_t<F>>{
      std::move(context), std::forward<F>(function), std::move(executor)};
}
/** @brief 不可取消的清理请求；只用于 close 等必须排空的受控系统调用。 */
template <class F>
  requires std::invocable<std::decay_t<F> &>
auto execute_cleanup(io::io_context context, F &&function) {
  auto service = context.cleanup();
  return execute_awaiter<std::decay_t<F>>{
      std::move(context), std::forward<F>(function), std::move(service), false};
}
} // namespace faio::execution
