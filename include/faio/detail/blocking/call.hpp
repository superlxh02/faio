/**
 * @file faio/detail/blocking/call.hpp
 * @brief 阻塞调用等待器，将独立线程执行的同步调用接入协程等待。
 */
#pragma once

#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/runtime/common/coroutine_integration.hpp"
#include "faio/detail/coroutine/context/task_context.hpp"
#include "faio/detail/coroutine/scheduler/resume_target.hpp"
#include "faio/detail/blocking/service.hpp"
#include "faio/detail/io/core/context.hpp"
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
#include <utility>
#include <variant>

namespace faio::blocking {
namespace detail {
/** @brief 提取 expected 结果类型的类型特征。 */
template <class T>
struct unwrap_expected {
  using type = T;                        /**< 类型变换后的目标类型。 */
  static constexpr bool wrapped = false; /**< 输入是否已经是 expected，避免再套一层错误包装。 */
};

/** @brief 提取 expected 结果类型的类型特征。 */
template <class T>
struct unwrap_expected<expected<T>> {
  using type = T;                       /**< 类型变换后的目标类型。 */
  static constexpr bool wrapped = true; /**< 输入是否已经是 expected，避免再套一层错误包装。 */
};

/**  @brief 地址固定的任务状态，线程池、等待者和 stop_callback 共享同一个完成状态。 */
template <class F, class service_type = service_ref>
struct call_state {
  using raw_result = std::invoke_result_t<F&>; /**< 调用函数未经展开的原始返回类型。 */
  using result_type = typename unwrap_expected<
      raw_result>::type; /**< 对外领取的结果类型，已去除内部 expected 包装。 */
  static_assert(!std::is_reference_v<result_type>, "阻塞服务不能返回借用引用");

  /** @brief 协作取消回调。 */
  struct cancel_callback {
    call_state* state; /**< 借用操作状态，外部所有者须保证其在使用期间存活。 */

    void operator()() const noexcept {
      std::lock_guard lock(state->mutex);
      if (!state->terminal && !state->cancellation_reason)
        state->cancellation_reason = state->timed_out() ? ETIMEDOUT : ECANCELED;
    }
  };

  io::io_context context; /**< 保持所属 IO 控制块和服务租约存活。 */
  service_type service;   /**< 按请求选择文件、DNS 或清理服务。 */
  bool cancellable{true}; /**< close 等清理操作必须执行，不能因父级停止令牌而跳过。 */
  F function;             /**< 提交前拥有完整系统调用参数，不借用临时 lambda。 */
  std::optional<expected<result_type>> result; /**< 完成仲裁后写一次。 */
  std::atomic<unsigned char> handshake{0};     /**< 0 提交中，1 已挂起，2 已完成。 */
  std::coroutine_handle<> continuation; /**< 操作安全完成后应恢复的协程句柄，不负责销毁父协程。 */
  std::mutex mutex;                     /**< 只仲裁取消与结果，不在执行系统调用时持锁。 */
  std::optional<int>
      cancellation_reason; /**< 保留首次停止原因，随后到达截止时间也不会覆盖已有取消原因。 */
  bool terminal{};         /**< 结果发布以后，迟到取消不再修改结果。 */
  std::optional<std::chrono::steady_clock::time_point>
      deadline; /**< 本次操作可选的单调时钟绝对截止时间。 */
  /** @brief 回调必须先注销，确保在途回调结束后再析构它依赖的状态。 */
  std::optional<std::stop_callback<cancel_callback>>
      stop_callback; /**< 按需就地保存的停止回调，析构时注销并等待在途回调结束。 */
  std::optional<std::stop_callback<cancel_callback>>
      domain_stop_callback; /**< 订阅 IO 域停止的回调；注销前域与操作状态均须有效。 */

  /** @brief 创建阻塞调用的共享状态。 */
  template <class Function>
  call_state(io::io_context ctx, Function&& operation, service_type service, bool can_cancel)
      : context(std::move(ctx)),
        service(std::move(service)),
        cancellable(can_cancel),
        function(std::forward<Function>(operation)) {}

  /** @brief 判断操作是否因超时结束。 */
  bool timed_out() const noexcept {
    return cancellable && deadline && std::chrono::steady_clock::now() >= *deadline;
  }

  /**  @brief 在 mutex 保护下记录截止时间，保留先发生的停止原因。 */
  void observe_deadline() noexcept {
    if (!cancellation_reason && timed_out())
      cancellation_reason = ETIMEDOUT;
  }

  /**  @brief 完成与取消在同一个短临界区内决定终态，协程只通过保存的调度器恢复。 */
  template <class Scheduler>
  void complete(expected<result_type> value, Scheduler scheduler) noexcept {
    {
      std::lock_guard lock(mutex);
      observe_deadline();
      if (cancellation_reason) {
        std::size_t progress = 0;
        if (!value)
          progress = value.error().progress();
        else if constexpr (std::integral<result_type>)
          progress = static_cast<std::size_t>(*value);
        value = std::unexpected{error{*cancellation_reason, progress}};
      }
      result.emplace(std::move(value));
      terminal = true;
    }
    if (handshake.exchange(2, std::memory_order_acq_rel) == 1)
      scheduler.schedule(continuation);
  }

  /** @brief 在独立阻塞线程上执行持有的同步函数；开始前复查停止状态，函数返回后才发布完成。 */
  template <class Scheduler>
  void run(Scheduler scheduler) noexcept {
    {
      std::lock_guard lock(mutex);
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
        complete(std::invoke(function), scheduler);
      else if constexpr (std::is_void_v<raw_result>) {
        std::invoke(function);
        complete(expected<void>{}, scheduler);
      } else
        complete(expected<result_type>{std::invoke(function)}, scheduler);
    } catch (const std::filesystem::filesystem_error& error) {
      complete(std::unexpected{make_error(error.code().value())}, scheduler);
    } catch (const std::bad_alloc&) {
      complete(std::unexpected{make_error(ENOMEM)}, scheduler);
    } catch (...) {
      complete(std::unexpected{make_error(EIO)}, scheduler);
    }
  }
};
}  // namespace detail

/**  @brief 类型化阻塞请求的协程桥接，构造时不提交、不执行系统调用。 */
template <class F, class service_type = service_ref>
class call_awaiter {
 public:
  using state_type = detail::call_state<F, service_type>; /**< 本桥接对象使用的稳定共享状态类型。 */
  using result_type =
      typename state_type::result_type; /**< 对外领取的结果类型，已去除内部 expected 包装。 */

  /** @brief 构造阻塞调用等待器。 */
  template <class Function>
    requires std::constructible_from<F, Function&&>
  call_awaiter(io::io_context context,
               Function&& function,
               service_type service,
               bool cancellable = true)
      : state_(std::make_shared<state_type>(std::move(context),
                                            std::forward<Function>(function),
                                            std::move(service),
                                            cancellable)) {}

  /** @brief 禁止复制构造阻塞调用等待器。 */
  call_awaiter(const call_awaiter&) = delete;

  /** @brief 禁止复制赋值。 */
  call_awaiter& operator=(const call_awaiter&) = delete;

  /** @brief 移动构造阻塞调用等待器。 */
  call_awaiter(call_awaiter&&) = default;

  /**  @brief 设置相对超时；借用型操作须等待已开始的系统调用结束后才返回。 */
  template <class Rep, class Period>
  call_awaiter& set_timeout(std::chrono::duration<Rep, Period> duration) & {
    state_->deadline = std::chrono::steady_clock::now() + duration;
    return *this;
  }

  /** @brief 把相对时长转换为单调时钟截止时间并保存到本次等待。 */
  template <class Rep, class Period>
  call_awaiter&& set_timeout(std::chrono::duration<Rep, Period> duration) && {
    set_timeout(duration);
    return std::move(*this);
  }

  /** @brief 保存单调时钟的绝对截止时间，由提交和驱动协议仲裁超时。 */
  call_awaiter& set_timeout_at(std::chrono::steady_clock::time_point deadline) & {
    state_->deadline = deadline;
    return *this;
  }

  /** @brief 保存单调时钟的绝对截止时间，由提交和驱动协议仲裁超时。 */
  call_awaiter&& set_timeout_at(std::chrono::steady_clock::time_point deadline) && {
    set_timeout_at(deadline);
    return std::move(*this);
  }

  /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
  bool await_ready() const noexcept { return false; }

  /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    auto state = state_;
    state->continuation = continuation;
    auto scheduler = [] {
      if constexpr (::faio::detail::has_task_scheduler_type<Promise>)
        return ::faio::detail::capture_current_resume_target<typename Promise::scheduler_type>();
      else
        return ::faio::detail::capture_resume_target(::faio::detail::current_scheduler());
    }();
    std::stop_token token;
    if constexpr (requires { continuation.promise().context.stop_token; }) {
      token = continuation.promise().context.stop_token;
    } else {
      token = ::faio::detail::current_task_context().stop_token;
    }
    if (!scheduler)
      throw std::logic_error("阻塞 IO 必须由运行时调度");
    if (state->cancellable) {
      if (token.stop_possible())
        state->stop_callback.emplace(token, typename state_type::cancel_callback{state.get()});
      auto domain_token = state->context.stop_token();
      if (domain_token.stop_possible())
        state->domain_stop_callback.emplace(domain_token,
                                            typename state_type::cancel_callback{state.get()});
    }
    if (!state->cancellable) {
      state->context.defer_cleanup([state, scheduler] { state->run(scheduler); });
    } else {
      auto accepted = state->service.try_submit([state, scheduler] { state->run(scheduler); });
      if (!accepted)
        state->complete(std::unexpected{accepted.error()}, scheduler);
    }
    unsigned char submitting = 0;
    return state->handshake.compare_exchange_strong(submitting, 1, std::memory_order_acq_rel);
  }

  /** @brief 取得异步操作结果。 */
  expected<result_type> await_resume() {
    state_->stop_callback.reset();
    state_->domain_stop_callback.reset();
    return std::move(state_->result.value());
  }

 private:
  std::shared_ptr<state_type>
      state_; /**< 共享持有操作状态，保证其在异步执行和领取结果期间地址不变。 */
};

/** @brief 构造阻塞调用等待器。 */
template <class F, class service_type>
call_awaiter(io::io_context, F&&, basic_service_ref<service_type>, bool = true)
    -> call_awaiter<std::decay_t<F>, basic_service_ref<service_type>>;

/**  @brief 向 context 的文件服务提交受控系统调用，自动展开 expected 返回值。 */
template <class F>
  requires std::invocable<std::decay_t<F>&>
auto call(io::io_context context, F&& function) {
  auto service = context.blocking();
  return call_awaiter<std::decay_t<F>>{
      std::move(context), std::forward<F>(function), std::move(service)};
}

/**  @brief 显式选择阻塞服务，例如解析器服务；操作持有参数和完成状态的所有权。 */
template <class F, class service_type>
  requires std::invocable<std::decay_t<F>&>
auto call_on(io::io_context context, F&& function, basic_service_ref<service_type> service) {
  return call_awaiter<std::decay_t<F>, basic_service_ref<service_type>>{
      std::move(context), std::forward<F>(function), std::move(service)};
}

/**  @brief 不可取消的清理请求；只用于 close 等必须排空的受控系统调用。 */
template <class F>
  requires std::invocable<std::decay_t<F>&>
auto call_cleanup(io::io_context context, F&& function) {
  auto service = context.cleanup();
  return call_awaiter<std::decay_t<F>>{
      std::move(context), std::forward<F>(function), std::move(service), false};
}
}  // namespace faio::blocking
