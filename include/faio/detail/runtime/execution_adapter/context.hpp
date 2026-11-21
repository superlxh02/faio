/**
 * @file faio/detail/runtime/execution_adapter/context.hpp
 * @brief 运行时上下文对 execution 任务图的接纳与派生任务接入。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/runtime/common/blocking_task.hpp"
#include "faio/detail/runtime/execution_adapter/external_work.hpp"

namespace faio::runtime::detail {
/** @brief 运行时与外部执行图的连接适配器。 */
class runtime_execution_adapter {
 public:
  /** @brief 取得关联的外部执行宿主。 */
  ::faio::detail::external_work_host& external_host(this auto& self) {
    if (self.current_)
      return self.current_->external_host();
    if (self.poller_)
      return self.poller_->shared()->external_host();
    throw std::logic_error("runtime_context 已停止");
  }

  /** @brief 取得外部根任务的调度器。 */
  scheduler_ref scheduler_for_external_root(this auto& self,
                                            const ::faio::detail::external_root_handle& root) {
    if (!self.external_host().owns(root))
      throw std::logic_error("external root does not belong to this runtime");
    return self.current_ ? self.current_->scheduler()
                         : self.poller_->shared()->scheduler_reference();
  }

  /** @brief 取得外部根任务的生命周期服务。 */
  task_lifetime_ref task_lifetime_for_external_root(
      this auto& self, const ::faio::detail::external_root_handle& root) {
    if (!self.external_host().owns(root))
      throw std::logic_error("external root does not belong to this runtime");
    return self.task_lifetime();
  }

 protected:
  /** @brief 向执行图提交阻塞调用。 */
  template <class S, class Self, class F>
  auto submit_blocking_on_graph(this Self& self, F&& function) {
    auto& host = self.external_host();
    const bool same_host = ::faio::detail::current_external_host() == &host;
    auto root = same_host ? host.lease(::faio::detail::current_task_context().external_scope)
                          : ::faio::detail::external_root_handle{};
    if (!root && !self.running())
      throw std::logic_error("runtime_context 已停止");
    const auto target = root ? self.scheduler_for_external_root(root) : self.scheduler();
    const bool inherit = same_host && ::faio::detail::current_scheduler() == target;
    auto lifetime = root ? self.task_lifetime_for_external_root(root) : self.task_lifetime();
    auto& pool = self.current_ ? self.current_->blocking() : self.poller_->shared()->blocking();
    return submit_blocking_task<S>(
        pool,
        std::forward<F>(function),
        inherit ? ::faio::detail::current_task_context().scope : nullptr,
        inherit ? ::faio::detail::current_task_context().stop_token : self.stop_.get_token(),
        lifetime,
        target,
        &host);
  }
};
}  // namespace faio::runtime::detail
#endif
