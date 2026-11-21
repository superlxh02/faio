/**
 * @file faio/detail/coroutine/context/task_context.hpp
 * @brief 随任务帧保存和继承的纯值执行属性。
 */
#ifndef FAIO_DETAIL_COROUTINE_TASK_CONTEXT_HPP
#define FAIO_DETAIL_COROUTINE_TASK_CONTEXT_HPP

#include "faio/detail/coroutine/scheduler/policy.hpp"
#include "faio/detail/coroutine/lifetime/tracker_fwd.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/external_work_types.hpp"
#endif
#include <stop_token>
#include <memory>
#include <optional>
#include <utility>

namespace faio {
namespace detail {
enum class cancellation_error_policy { fatal_if_unobserved, normal_if_stop_requested };

/** @brief 此所有者仅维持取消回调链，不持有协程帧或结果；保留祖先回调不会通过子任务结果形成所有权环。 */
struct cancellation_state {
  /** @brief 父任务停止请求转发器。 */
  struct forward {
    std::stop_source* target; /**< 接收父级停止传播的目标停止源，回调期间保持有效。 */

    void operator()() const noexcept { target->request_stop(); }
  };

  std::shared_ptr<cancellation_state> parent; /**< 保持父级取消连接的共享状态。 */
  std::stop_source source; /**< 本操作或任务图的停止源，向已登记等待者广播协作取消。 */
  std::optional<std::stop_callback<forward>>
      link; /**< 父停止令牌到当前停止源的回调连接，析构时注销。 */

  /** @brief 构造取消状态。 */
  cancellation_state(std::stop_token token, std::shared_ptr<cancellation_state> owner)
      : parent(std::move(owner)) {
    if (token.stop_possible())
      link.emplace(token, forward{&source});
  }
};

/** @brief 持有停止源及可选的单向父连接，清理分支用空父令牌建立独立状态。 */
using linked_stop_state = cancellation_state;

/**
 * @brief 保存在用户 task promise 中的执行属性。
 * 父任务在 await_suspend 中传递停止请求和作用域，父子直接 co_await 时按值继承。
 * 调度器与 worker_id 从当前执行线程查询，挂起操作单独捕获自身的恢复目标。
 * tracker 指针仅借用外部对象。
 */
struct task_context {
  ::faio::detail::task_tracker*
      scope{}; /**< 借用当前任务所属的根任务计数组，恢复 TLS 后供 spawn 派生的根任务登记。 */
  std::stop_token
      stop_token{}; /**< 本任务的停止令牌，继承自父任务或根任务停止源，协程迁移后仍关联同一停止状态。 */
  cancellation_error_policy cancellation_policy{
      cancellation_error_policy::
          fatal_if_unobserved};        /**< 未观察的取消异常处理策略，随任务上下文传播。 */
  task_scheduling_policy scheduling{}; /**< 启动时解析的有效调度约束，父子按值继承。 */
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  external_scope_ref external_scope{};
#endif
  std::shared_ptr<cancellation_state>
      cancellation_owner{}; /**< 持有取消回调链的共享状态，不持有协程结果或协程帧。 */
};

/** @brief task_context 的兼容别名；调度器类型约束由 task、promise 和等待协议检查。 */
template <class S>
using basic_task_context = task_context;

}  // namespace detail
}  // namespace faio
#endif
