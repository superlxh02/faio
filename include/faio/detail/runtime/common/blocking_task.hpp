/**
 * @file faio/detail/runtime/common/blocking_task.hpp
 * @brief 阻塞任务完成状态。
 */
#ifndef FAIO_DETAIL_RUNTIME_COMMON_BLOCKING_TASK_HPP
#define FAIO_DETAIL_RUNTIME_COMMON_BLOCKING_TASK_HPP

#include "faio/detail/coroutine/context/context_scope.hpp"
#include "faio/detail/coroutine/lifetime/reference.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/blocking_task.hpp"
#endif
#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/composition/join_handle.hpp"
#include "faio/detail/coroutine/lifetime/tracker.hpp"
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
template <class F, class S = ::faio::default_scheduler>
struct blocking_task_state : ::faio::detail::join_handle_state<std::invoke_result_t<F&>, S>
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    ,
                             blocking_task_execution
#endif
{
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  using blocking_task_execution::host;
  using blocking_task_execution::external_scope;
  using blocking_task_execution::external_child;
#endif
  using result_type = std::invoke_result_t<F&>;
  static constexpr bool inline_function =
      sizeof(F) <= ::faio::move_only_function<void()>::inline_capacity
      && alignof(F) <= alignof(std::max_align_t);
  using function_storage =
      std::conditional_t<inline_function, std::optional<F>, std::unique_ptr<F>>;

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
                      ::faio::scheduler_ref_for<S> scheduler
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                      ,
                      ::faio::detail::external_work_host* external_host
#endif
                      )
      :
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
        blocking_task_execution(external_host),
#endif
        function(store_function(std::forward<Function>(callable))),
        pool(service),
        tracker(scope),
        lifetime(owner),
        target(scheduler) {
  }

  void run() {
    {
      ::faio::detail::scoped_submission_binding submission{target,
                                                           lifetime,
                                                           pool
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                                                           ,
                                                           host
#endif
      };
      ::faio::detail::scoped_task_context context{
          ::faio::detail::task_context{.scope = tracker,
                                       .stop_token = this->stop_source.get_token(),
                                       .cancellation_policy = this->cancellation_policy,
                                       .scheduling = scheduling,
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                                       .external_scope = external_scope,
#endif
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
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
      auto completion =
          tracker ? tracker->done() : ::faio::detail::task_tracker::completion_guard{};
#else
      if (tracker)
        tracker->done();
#endif
      lifetime.finish_task();
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
      external_child.reset();
#endif
    }
    function.reset();
  }

  ::faio::detail::task_scheduling_policy scheduling{};
  function_storage function;
  blocking_pool& pool;
  ::faio::detail::task_tracker* tracker;
  ::faio::task_lifetime_ref lifetime;
  ::faio::scheduler_ref_for<S> target;
};

/** @brief 向阻塞池提交同步函数，保存结果并登记任务组与运行时生命周期。 */
template <class S = ::faio::default_scheduler, class F>
  requires std::invocable<std::decay_t<F>&>
auto submit_blocking_task(
    blocking_pool& pool,
    F&& function,
    ::faio::detail::task_tracker* tracker,
    std::stop_token parent_stop,
    ::faio::task_lifetime_ref lifetime,
    ::faio::scheduler_ref_for<S> target = ::faio::detail::current_scheduler_for<S>()
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
        ,
    ::faio::detail::external_work_host* host = ::faio::detail::current_external_host()
#endif
        ) -> join_handle<std::invoke_result_t<std::decay_t<F>&>, S> {
  using result_type = std::invoke_result_t<std::decay_t<F>&>;
  static_assert(!std::is_reference_v<result_type>, "阻塞任务不能返回引用");
  using state_type = blocking_task_state<std::decay_t<F>, S>;
  auto state = std::make_shared<state_type>(std::forward<F>(function),
                                            pool,
                                            tracker,
                                            lifetime,
                                            target
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                                            ,
                                            host
#endif
  );
  const bool same_host = target && ::faio::detail::current_scheduler_matches(target)
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                         && host == ::faio::detail::current_external_host()
#endif
      ;
  state->scheduling = same_host ? ::faio::detail::current_task_context().scheduling
                                : ::faio::detail::task_scheduling_policy{};
  state->cancellation_policy = same_host
                                   ? ::faio::detail::current_task_context().cancellation_policy
                                   : ::faio::detail::cancellation_error_policy::FATAL_IF_UNOBSERVED;
  if (state->cancellation_policy
          == ::faio::detail::cancellation_error_policy::NORMAL_IF_STOP_REQUESTED
      || (same_host && ::faio::detail::current_task_context().cancellation_owner)) {
    state->cancellation_owner = std::make_shared<::faio::detail::cancellation_state>(
        parent_stop,
        same_host ? ::faio::detail::current_task_context().cancellation_owner : nullptr);
    state->stop_source = state->cancellation_owner->source;
  }
  if (!state->cancellation_owner && parent_stop.stop_possible())
    state->parent_callback.emplace(parent_stop, ::faio::detail::forward_stop{&state->stop_source});
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  state->inherit_scope(same_host);
#endif

  lifetime.register_task();
  if (tracker)
    tracker->add();
  try {
    pool.submit([state] { state->run(); });
  } catch (...) {
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    auto completion = tracker ? tracker->done() : ::faio::detail::task_tracker::completion_guard{};
#else
    if (tracker)
      tracker->done();
#endif
    lifetime.finish_task();
    throw;
  }
  return join_handle<result_type, S>{std::move(state)};
}
}  // namespace faio::runtime::detail
#endif
