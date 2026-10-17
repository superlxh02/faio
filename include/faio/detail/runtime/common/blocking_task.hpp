/**
 * @file faio/detail/runtime/common/blocking_task.hpp
 * @brief 运行时调度、线程协调与定时驱动：阻塞任务完成状态。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#ifndef FAIO_DETAIL_RUNTIME_COMMON_BLOCKING_TASK_HPP
#define FAIO_DETAIL_RUNTIME_COMMON_BLOCKING_TASK_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/join_handle.hpp"
#include "faio/detail/coroutine/task_lifetime_ref.hpp"
#include "faio/detail/coroutine/task_tracker.hpp"
#include "faio/detail/runtime/common/blocking_pool.hpp"
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace faio::runtime::detail {
/// @brief 完成状态与小型阻塞任务参数共用一次分配；队列只持有 shared_ptr 回调。
/// 句柄按基类协议观察结果，共享控制块仍按真实类型销毁完整状态。
template <class F>
struct blocking_task_state : ::faio::detail::join_handle_state<std::invoke_result_t<F&>> {
  using result_type = std::invoke_result_t<F&>;  ///< 对外领取的结果类型，已去除内部 expected 包装。
  static constexpr bool inline_function =
      sizeof(F) <= ::faio::move_only_function<void()>::inline_capacity
      && alignof(F) <= alignof(std::max_align_t);  ///< 大捕获单独存储，完成后可释放大块内存，不被长期保留的结果句柄拖住。
  using function_storage =
      std::conditional_t<inline_function, std::optional<F>, std::unique_ptr<F>>;  ///< 按捕获大小与移动保证选择的内联或堆存储类型。

  template <class Function>
  static function_storage store_function(Function&& callable) {
    if constexpr (inline_function)
      return std::optional<F>{std::in_place, std::forward<Function>(callable)};
    else
      return std::make_unique<F>(std::forward<Function>(callable));
  }

  template <class Function>
  blocking_task_state(Function&& callable,
                      blocking_pool& service,
                      ::faio::detail::task_tracker* scope,
                      ::faio::task_lifetime_ref owner,
                      ::faio::scheduler_ref scheduler,
                      ::faio::detail::external_work_host* external_host)
      : function(store_function(std::forward<Function>(callable))),
        pool(service),
        tracker(scope),
        lifetime(owner),
        target(scheduler),
        host(external_host) {}

  void run() {
    {
      ::faio::detail::scoped_submission_binding submission{target, lifetime, pool, host};
      ::faio::detail::scoped_task_context context{{.scope = tracker,
                                                   .scheduler = target,
                                                   .stop_token = this->stop_source.get_token(),
                                                   .cancellation_policy = this->cancellation_policy,
                                                   .external_scope = external_scope,
                                                   .cancellation_owner = this->cancellation_owner}};
      try {
        if (this->stop_source.stop_requested())
          throw operation_cancelled{};
        if constexpr (std::is_void_v<result_type>) {
          std::invoke(*function);
          this->value.emplace();
        } else {
          this->value.emplace(std::invoke(*function));
        }
      } catch (...) {
        this->capture_error(std::current_exception());
      }
      this->publish();
      auto completion =
          tracker ? tracker->done() : ::faio::detail::task_tracker::completion_guard{};
      lifetime.finish_task();
      external_child.reset();
    }
    /// @brief 结果句柄可能长期保留；任务完成后立即释放用户捕获的资源。
    /// 与原队列回调析构一样，用户捕获的析构不继承已完成任务的 TLS 上下文。
    function.reset();
  }

  function_storage function;  ///< 拥有的待执行可调用对象，提交后由阻塞服务线程执行。
  blocking_pool& pool;  ///< 借用执行该同步调用的阻塞任务池。
  ::faio::detail::task_tracker* tracker;  ///< 跟踪本任务及派生任务的完成计数，归零后才允许排空返回。
  ::faio::task_lifetime_ref lifetime;  ///< 根任务计数服务的借用引用，接纳与完成必须成对记账。
  ::faio::scheduler_ref target;  ///< 借用所属对象的稳定地址，回调或等待期间不得迁移或销毁。
  ::faio::detail::external_work_host* host;  ///< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。
  ::faio::detail::external_scope_ref external_scope;  ///< 当前外部任务图的作用域身份，派生任务沿用该归属。
  ::faio::detail::external_child_lease external_child;  ///< 外部任务图的子任务租约，覆盖当前根协程的执行和清理。
};

/// @brief 阻塞任务不创建协程帧；其完成状态与派生协程任务使用相同的可等待协议。
template <class F>
  requires std::invocable<std::decay_t<F>&>
auto start_blocking(
    blocking_pool& pool,
    F&& function,
    ::faio::detail::task_tracker* tracker,
    std::stop_token parent_stop,
    ::faio::task_lifetime_ref lifetime,
    ::faio::scheduler_ref target = ::faio::detail::current_scheduler(),
    ::faio::detail::external_work_host* host = ::faio::detail::current_external_host())
    -> join_handle<std::invoke_result_t<std::decay_t<F>&>> {
  using result_type = std::invoke_result_t<std::decay_t<F>&>;
  static_assert(!std::is_reference_v<result_type>, "阻塞任务不能返回引用");
  using state_type = blocking_task_state<std::decay_t<F>>;
  auto state = std::make_shared<state_type>(
      std::forward<F>(function), pool, tracker, lifetime, target, host);
  const bool same_host = target && host == ::faio::detail::current_external_host()
                         && target == ::faio::detail::current_scheduler();
  state->cancellation_policy = same_host
                                   ? ::faio::detail::current_cancellation_policy
                                   : ::faio::detail::cancellation_error_policy::fatal_if_unobserved;
  const auto external_scope = same_host && ::faio::detail::current_external_scope.valid()
                                      && ::faio::detail::current_external_scope.host == host
                                  ? ::faio::detail::current_external_scope
                                  : ::faio::detail::external_scope_ref{};
  if (state->cancellation_policy
          == ::faio::detail::cancellation_error_policy::normal_if_stop_requested
      || (same_host && ::faio::detail::current_cancellation_owner)) {
    state->cancellation_owner = std::make_shared<::faio::detail::cancellation_state>(
        parent_stop, same_host ? ::faio::detail::current_cancellation_owner : nullptr);
    state->stop_source = state->cancellation_owner->source;
  }
  if (!state->cancellation_owner && parent_stop.stop_possible())
    state->parent_callback.emplace(parent_stop, ::faio::detail::forward_stop{&state->stop_source});
  state->external_scope = external_scope;
  state->external_child = external_scope.valid() ? host->acquire_child(external_scope)
                                                 : ::faio::detail::external_child_lease{};

  /// @brief 所有可能抛出的参数构造均已完成，再登记运行时生命周期计数。
  lifetime.register_task();
  if (tracker)
    tracker->add();
  try {
    pool.submit([state] { state->run(); });
  } catch (...) {
    auto completion = tracker ? tracker->done() : ::faio::detail::task_tracker::completion_guard{};
    lifetime.finish_task();
    throw;
  }
  return join_handle<result_type>{std::move(state)};
}
}
#endif
