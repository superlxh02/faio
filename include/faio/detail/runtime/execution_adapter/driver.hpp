/**
 * @file faio/detail/runtime/execution_adapter/driver.hpp
 * @brief execution 节点与原生协程交替调度的运行时接入。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/external_work.hpp"
#include "faio/detail/coroutine/context/context_scope.hpp"

namespace faio::runtime::detail {
/** @brief 每个驱动线程拥有独立的公平性状态，宿主共享接纳与排空责任。 */
class execution_driver {
 public:
  /** @brief 读取优先恢复线程。 */
  bool preferred(::faio::detail::external_work_host& host, std::size_t worker) const noexcept {
    return prefer_external_ && host.has_ready(worker);
  }

  /** @brief 读取原生操作完成进度。 */
  void native_progress() noexcept { prefer_external_ = true; }

  /** @brief 执行已领取的任务。 */
  template <class LocalScheduler>
  void execute(::faio::detail::external_work_host& host,
               LocalScheduler& local,
               std::size_t worker) noexcept {
    auto dispatch = host.try_pop_external(worker);
    if (!dispatch)
      return;
    local.before_execute();
    local.interrupt_execution_chain();
    ::faio::detail::cooperative_poll_scope budget;
    ::faio::detail::scoped_task_context context{
        {.stop_token = dispatch.root() ? dispatch.root()->stop_token() : std::stop_token{},
         .external_scope = dispatch.scope()}};
    dispatch.node()->execute();
    prefer_external_ = false;
  }

 private:
  bool prefer_external_{true};
};
}  // namespace faio::runtime::detail
#endif
