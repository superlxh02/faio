/**
 * @file faio/detail/coroutine/task/promise.hpp
 * @brief task 的 promise、结果槽与首次及最终挂起协议。
 */
#ifndef FAIO_DETAIL_COROUTINE_TASK_PROMISE_HPP
#define FAIO_DETAIL_COROUTINE_TASK_PROMISE_HPP

#include "faio/detail/coroutine/task/forward.hpp"
#include "faio/detail/coroutine/memory/frame_allocator.hpp"
#include "faio/detail/coroutine/context/task_context.hpp"
#include "faio/detail/coroutine/lifecycle/owned_hooks.hpp"
#include "faio/detail/coroutine/task/await_context.hpp"
#include "faio/detail/coroutine/context/this_coro.hpp"
#include "faio/detail/coroutine/context/scheduling.hpp"
#include <concepts>
#include <coroutine>
#include <exception>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

#if defined(FAIO_EXPERIMENTAL_TASK_LIFECYCLE) && FAIO_EXPERIMENTAL_TASK_LIFECYCLE
#include "faio/detail/experimental/lifecycle.hpp"
#endif

namespace faio::detail {
/** @brief 有结果和 void 任务共用的 promise：保存续体、异常及执行属性。 */
template <class S>
struct basic_task_promise_base {
  using faio_task_promise_tag = void;
  using scheduler_type = S; /**< 静态调度器约束，不在协程帧保存调度器实例。 */

  std::coroutine_handle<> continuation{}; /**< 本任务结束后接续执行的父协程句柄，不拥有父帧。 */
  std::exception_ptr exception{}; /**< 离开协程体的未处理异常，延迟到父任务领取结果时重抛。 */
  task_context context{};         /**< 此任务随帧保存的任务组、停止与有效调度属性。 */
  task_scheduling_options scheduling_options{}; /**< 只属于此任务的显式请求，不随父上下文复制。 */
  bool scheduling_initialized{}; /**< 有效策略只在首次启动解析，挂起恢复不重复告警。 */
  owned_task_lifecycle
      lifecycle_hooks; /**< 只保存一个拥有辅助帧的句柄；未注解任务不保存策略或退出状态。 */

  /** @brief 固定核心的桥接接口只接收普通构造描述，不在基类捕获 current_function。 */
  explicit basic_task_promise_base(task_lifecycle_configuration configuration = {})
      : lifecycle_hooks(configuration.factory ? configuration.factory() : owned_task_lifecycle{}) {}

  /** @brief 首次执行前解析调度选项并保存有效策略。 */
  void initialize_scheduling_opt(const task_scheduling_policy& inherited) {
    if (scheduling_initialized)
      return;
    if (scheduling_options.stealing != scheduling_setting::inherit
        || scheduling_options.bind_core != scheduling_setting::inherit || inherited.no_stealing
        || inherited.binds_core()) [[unlikely]] {
      context.scheduling = resolve_task_scheduling<S>(scheduling_options, inherited);
    } else if (&context.scheduling != &inherited) {
      context.scheduling = inherited;
    }
    scheduling_initialized = true;
  }

  /** @brief 等待表达式转换时：将停止令牌查询标记转换为立即完成的 awaiter。 */
  auto await_transform(this_coro::stop_token_t) noexcept {
    return query_awaiter<std::stop_token>{context.stop_token};
  }

  /** @brief 等待表达式转换时：读取当前执行线程的具体调度借用。 */
  auto await_transform(this_coro::scheduler_t) {
    return query_awaiter<scheduler_ref_for<S>>{require_current_scheduler_for<S>()};
  }

  /** @brief 等待表达式转换时：创建动态 worker 查询器，恢复时读取所在 worker。 */
  auto await_transform(this_coro::worker_id_t) noexcept { return worker_id_awaiter{}; }

  /** @brief 等待表达式转换时：构造会主动重新排队的让出操作。 */
  auto await_transform(this_coro::yield_t) {
    return scoped_awaiter<basic_yield_awaiter<S>, S>{
        basic_yield_awaiter<S>{
            capture_current_resume_target<S>(), context.scope, &context.stop_token},
        &context};
  }

  /** @brief 等待表达式转换时：构造使用本轮实际恢复共享预算的条件让出操作。 */
  auto await_transform(this_coro::yield_if_needed_t) noexcept {
    return basic_yield_if_needed_awaiter<S>{&context};
  }

  /** @brief 等待表达式转换时：保留原生 awaitable，并添加任务上下文恢复适配。 */
  template <class A>
  auto await_transform(A&& value) {
    if constexpr (requires {
                    std::forward<A>(value).template rebind_scheduler<S>(scheduler_ref_for<S>{});
                  }) {
      using rebound_t =
          decltype(std::forward<A>(value).template rebind_scheduler<S>(scheduler_ref_for<S>{}));
      return scoped_awaiter<rebound_t, S>{
          std::forward<A>(value).template rebind_scheduler<S>(require_current_scheduler_for<S>()),
          &context};
    } else {
      if constexpr (requires { typename std::remove_cvref_t<A>::scheduler_type; }) {
        static_assert(std::same_as<S, typename std::remove_cvref_t<A>::scheduler_type>,
                      "父子任务和同步操作必须使用相同的静态调度器类型");
      }
      using awaiter_t = decltype(as_awaiter(std::forward<A>(value)));
      using stored_t = scoped_awaiter_storage_t<awaiter_t>;
      return scoped_awaiter<stored_t, S>{as_awaiter(std::forward<A>(value)), &context};
    }
  }

  /** @brief 首次挂起时：保持惰性，创建 task 后不立即执行协程函数体。 */
  struct initial_awaiter {
    basic_task_promise_base* promise;

    /** @brief 首次执行前始终挂起。 */
    bool await_ready() const noexcept { return false; }

    /** @brief 保持首次挂起，等待外部恢复。 */
    void await_suspend(std::coroutine_handle<>) const noexcept {}

    /** @brief 首次恢复时运行 before；抛错交给原协程异常出口，任务体不会开始。 */
    void await_resume() const {
      if (!promise->scheduling_initialized || promise->lifecycle_hooks) [[unlikely]] {
        prepare_initial_resume(*promise);
        return;
      }
      restore_native_task_context(promise->context);
    }

   private:
#if __has_cpp_attribute(gnu::cold)
    [[gnu::cold]]
#endif
    static void prepare_initial_resume(basic_task_promise_base& promise) {
      if (!promise.scheduling_initialized)
        promise.initialize_scheduling_opt(current_task_context().scheduling);
      restore_native_task_context(promise.context);
      if (promise.lifecycle_hooks)
        promise.lifecycle_hooks.before();
    }
  };

  /** @brief 提供协程首次挂起策略。 */
  initial_awaiter initial_suspend() noexcept { return {this}; }

  /** @brief 无成员的最终挂起 awaiter，以对称转移把执行权直接交回父协程。 */
  struct final_awaiter {
    /** @brief 挂起前检查：任务结束时总进入最终挂起，等待持有者安全销毁子帧。 */
    bool await_ready() const noexcept { return false; }

    /** @brief 完成生命周期回调，并将执行权交回父续体。 */
    template <class Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> child) const noexcept {
      auto& promise = child.promise();
      if (promise.lifecycle_hooks && promise.scheduling_initialized) [[unlikely]]
        finish_lifecycle(promise);
      auto parent = promise.continuation;
      return parent ? parent : std::noop_coroutine();
    }

    /** @brief 恢复时：最终挂起不会被正常恢复，此函数只补齐 awaiter 协议。 */
    void await_resume() const noexcept {}

   private:
#if __has_cpp_attribute(gnu::cold)
    [[gnu::cold]]
#endif
    static void finish_lifecycle(basic_task_promise_base& promise) noexcept {
      restore_task_context(promise.context);
      promise.lifecycle_hooks.after();
    }
  };

  /** @brief 协程终止时交接恢复与销毁责任。 */
  final_awaiter final_suspend() const noexcept { return {}; }

  /** @brief 异常离开协程体时：保存异常，随后仍经过最终挂起回到父协程。 */
  void unhandled_exception() noexcept {
    exception = std::current_exception();
    lifecycle_hooks.record_exception(exception);
  }

  /** @brief 结果消费辅助接口：有已记录异常时重抛，否则允许读取返回值。 */
  void rethrow_if_failed() const {
    if (exception)
      std::rethrow_exception(exception);
  }
};

/** @brief 有返回值任务的 promise，在共用状态外增加一个尚未构造的结果槽。 */
using task_promise_base = basic_task_promise_base<default_scheduler>;

/** @brief 惰性协程任务的 promise 状态。 */
template <class T, class S = default_scheduler>
struct task_promise : basic_task_promise_base<S>, task_frame_allocation<task_promise<T, S>> {
  static_assert(!std::is_reference_v<T>, "task<T&> 会产生悬空引用；请返回值或引用包装器");
  std::optional<T> value; /**< co_return 之前为空；正常返回时原位构造 T，父任务消费时移动取出。 */

#if defined(FAIO_EXPERIMENTAL_TASK_LIFECYCLE) && FAIO_EXPERIMENTAL_TASK_LIFECYCLE
  /** @brief 通过普通参数的默认实参在实际 promise 构造调用处取得原协程函数，不能将其移到基类。 */
  task_promise(task_lifecycle_configuration configuration =
                   experimental::detail::task_lifecycle_hooks_for(std::meta::current_function()))
      : basic_task_promise_base<S>(configuration) {}
#else
  /** @brief 普通组件不启用反射，C++23/C++26 混编时使用同一个定义。 */
  task_promise() = default;
#endif

  /** @brief 创建协程时，从本 promise 构造独占协程帧且只能移动的 task 返回对象。 */
  task<T, S> get_return_object() noexcept;

  /** @brief 保存协程返回值。 */
  template <class U>
    requires std::constructible_from<T, U&&>
  void return_value(U&& result) {
    this->lifecycle_hooks.begin_result();
    value.emplace(std::forward<U>(result));
  }

  /** @brief 领取有返回值任务结果；必须在任务结束后调用，失败时先重抛异常。 */
  T take_result() {
    this->rethrow_if_failed();
    return std::move(value.value());
  }
};

/** @brief 无返回值任务的 promise，只复用异常与上下文，不存放用户结果。 */
template <class S>
struct task_promise<void, S> : basic_task_promise_base<S>,
                               task_frame_allocation<task_promise<void, S>> {
#if defined(FAIO_EXPERIMENTAL_TASK_LIFECYCLE) && FAIO_EXPERIMENTAL_TASK_LIFECYCLE
  /** @brief void 特化也在实际 promise 构造调用处读取原函数注解。 */
  task_promise(task_lifecycle_configuration configuration =
                   experimental::detail::task_lifecycle_hooks_for(std::meta::current_function()))
      : basic_task_promise_base<S>(configuration) {}
#else
  /** @brief 构造惰性协程任务的 promise 状态。 */
  task_promise() = default;
#endif
  /** @brief 创建协程时：从 void promise 构造对应 task<void>。 */
  task<void, S> get_return_object() noexcept;

  /** @brief 执行无值 co_return 或运行到函数末尾时：标记正常返回，无用户值需要存储。 */
  void return_void() noexcept {}

  /** @brief 领取 void 任务完成结果，仅检查并传播未处理异常。 */
  void take_result() { this->rethrow_if_failed(); }
};
}  // namespace faio::detail
#endif
