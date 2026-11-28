/**
 * @file faio/detail/coroutine/launch/root_task.hpp
 * @brief 根任务帧所有权、生命周期登记与调度入口。
 */
#pragma once

#include "faio/detail/coroutine/lifetime/reference.hpp"
#include "faio/detail/coroutine/context/execution_binding.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/execution_adapter/child_lifetime.hpp"
#endif
#include "faio/detail/coroutine/task/forward.hpp"
#include "faio/detail/coroutine/task/await_context.hpp"
#include "faio/detail/coroutine/context/task_context.hpp"
#include "faio/detail/coroutine/scheduler/resume_target.hpp"
#include "faio/detail/coroutine/lifetime/tracker.hpp"
#include "faio/detail/common/cancellation.hpp"
#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace faio::detail {
/**
 * @brief 根协程的帧在提交前由本对象持有，提交后由调度器接管，并在 final_suspend 的不挂起路径自动销毁。
 * 用户 task 则由父 awaiter 销毁帧，两种所有权模式分开管理。
 * promise_type 必须在协程函数定义处完整，因此仅前置声明 root_task 不够。
 */
class root_task {
 public:
  /** @brief 根协程 promise，保存结束时归还计数所需的借用目标，不保存用户结果。 */
  struct promise_type
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
      : /** @brief 根 promise 继承的外部子任务租约。 */
        execution_child_lifetime
#endif
  {
    ~promise_type() {
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
      auto completion = scope ? scope->done() : task_tracker::completion_guard{};
#else
      if (scope)
        scope->done();
#endif
      if (lifetime)
        lifetime.finish_task();
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
      finish_execution_child();
#endif
    }

    task_tracker* scope{};
    task_lifetime_ref lifetime{};
    task_context context{};

    /** @brief 创建协程时：生成提交前拥有根帧的 root_task 对象。 */
    root_task get_return_object() noexcept {
      return root_task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    /** @brief 根任务首次挂起时保持惰性，由调度入口完成登记后再投递。 */
    struct initial_awaiter {
      const promise_type* promise;

      /** @brief 始终保持首次挂起，等待调度入口投递。 */
      bool await_ready() const noexcept { return false; }

      /** @brief 保持首次挂起，此处不投递协程。 */
      void await_suspend(std::coroutine_handle<>) const noexcept {}

      /** @brief 首次恢复时安装根任务上下文。 */
      void await_resume() const noexcept { install_task_context(promise->context); }
    };

    /** @brief 提供协程首次挂起策略。 */
    initial_awaiter initial_suspend() const noexcept { return {this}; }

    /** @brief 根包装不固定静态调度器类型，只为等待操作恢复本根任务的完整属性。 */
    template <class A>
    auto await_transform(A&& value) {
      using awaiter_t = decltype(as_awaiter(std::forward<A>(value)));
      using stored_t = scoped_awaiter_storage_t<awaiter_t>;
      return scoped_awaiter<stored_t>{as_awaiter(std::forward<A>(value)), &context};
    }

    /** @brief 完成后直接销毁协程帧。 */
    std::suspend_never final_suspend() const noexcept { return {}; }

    /** @brief 执行无值 co_return 或运行到末尾时：正常结束，不保存用户返回值。 */
    void return_void() const noexcept {}

    /** @brief 未捕获异常时终止进程。 */
    void unhandled_exception() const noexcept { std::terminate(); }
  };

  using handle_type = std::coroutine_handle<promise_type>;

  /** @brief 接管新建的尚未提交根帧，不立即执行。 */
  explicit root_task(handle_type h) : handle_(h) {}

  /** @brief 移动根帧所有权并把源对象置空，保证仅一个提交前持有者。 */
  root_task(root_task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  /** @brief 禁止复制根帧句柄，避免重复提交与销毁。 */
  root_task(const root_task&) = delete;

  /** @brief 销毁仍由本对象持有的根帧；已转出的帧由接管方负责。 */
  ~root_task() {
    if (handle_)
      handle_.destroy();
  }

  /** @brief 向提交入口转出唯一根帧所有权，本对象之后不负责销毁。 */
  [[nodiscard]] handle_type release_handle() noexcept { return std::exchange(handle_, {}); }

  /** @brief 投递前绑定根帧析构时的计数归还目标，必须与调用者的登记配对。 */
  void set_completion(task_lifetime_ref lifetime, task_tracker* scope = nullptr) noexcept {
    handle_.promise().lifetime = lifetime;
    handle_.promise().scope = scope;
  }

  /** @brief 根帧发布前一次性配置任务属性，不把这些属性重复保存在协程参数中。 */
  void set_context(task_context context) noexcept {
    handle_.promise().context = std::move(context);
  }

  /** @brief 设置任务调度属性。 */
  void set_scheduling(task_scheduling_policy scheduling) noexcept {
    handle_.promise().context.scheduling = scheduling;
  }

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  /** @brief 保存外部执行子任务租约。 */
  void set_external_child(external_child_lease child) noexcept {
    handle_.promise().external_child = std::move(child);
  }
#endif

 private:
  handle_type handle_;
};

/**
 * @brief 登记根任务生命周期与完成归还目标，并返回符合调度约束的恢复目标。
 * 调用前外层 scope 计数已登记；交给调度器前仍由调用方持有根帧。
 */
template <class S = default_scheduler>
basic_resume_target<S> prepare_root_task(root_task& root,
                                         basic_scheduler_ref<S> scheduler,
                                         task_tracker* scope,
                                         task_lifetime_ref lifetime) {
  root.set_completion({}, scope);
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  bind_execution_child(root, scheduler);
#endif
  if (!scheduler)
    throw std::logic_error("没有可用的运行时");
  const auto inherited = current_scheduler_matches(scheduler) ? current_task_context().scheduling
                                                              : task_scheduling_policy{};
  const basic_resume_target<S> target{scheduler, inherited};
  root.set_scheduling(inherited);
  lifetime.register_task();
  root.set_completion(lifetime, scope);
  return target;
}

/** @brief 立即投递根协程，失败时销毁根帧并归还完成计数。 */
template <class S = default_scheduler>
void schedule_task(root_task root,
                   basic_scheduler_ref<S> scheduler,
                   task_tracker* scope = nullptr,
                   task_lifetime_ref lifetime = current_task_lifetime()) {
  const auto target = prepare_root_task(root, scheduler, scope, lifetime);
  auto handle = root.release_handle();
  try {
    target.schedule(handle);
  } catch (...) {
    handle.destroy();
    throw;
  }
}

/** @brief 无结果观察者的根包装，拥有 child；协作取消遵循任务策略，未处理异常终止进程。 */
template <class T, class S>
root_task unobserved_root_task_wrapper(task<T, S> child) {
  try {
    co_await std::move(child);
  } catch (const operation_cancelled&) {
    const auto& context = current_task_context();
    if (context.cancellation_policy != cancellation_error_policy::NORMAL_IF_STOP_REQUESTED
        || !context.stop_token.stop_requested())
      throw;
  }
}

/** @brief 调度不保留结果句柄的任务，并登记其所属任务组与运行时生命周期。 */
template <class T, class S>
void schedule_unobserved_task(scheduler_ref_for<S> scheduler,
                              task<T, S> child,
                              task_tracker* tracker = nullptr,
                              task_lifetime_ref lifetime = current_task_lifetime()) {
  const bool inherit = !has_current_scheduler() || current_scheduler_matches(scheduler);
  auto context = inherit ? capture_task_context() : task_context{};
  context.scope = tracker;
  auto root = unobserved_root_task_wrapper(std::move(child));
  root.set_context(std::move(context));
  if (tracker)
    tracker->add();
  schedule_task(std::move(root), scheduler, tracker, lifetime);
}
}  // namespace faio::detail
