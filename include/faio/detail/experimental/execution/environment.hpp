/**
 * @file faio/detail/experimental/execution/environment.hpp
 * @brief 发送器环境与停止传播。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_ENVIRONMENT_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_ENVIRONMENT_HPP
#include "faio/detail/experimental/execution/scheduler.hpp"

namespace faio::experimental::detail {
/** @brief 组合运行时调度器、停止令牌和原 receiver 环境。 */
template <faio::runtime::mode Mode, class Base = stdexec::env<>>
struct root_environment {
  execution_scheduler<Mode>
      scheduler;    /**< 保存目标调度器，跨线程完成时用它将续体投递回所属运行时。 */
  root_handle root; /**< 持有任务图的根状态租约，维持接纳、取消及排空的归属。 */
  Base base{};      /**< 底层接收器环境，保留原环境的查询行为。 */

  /** @brief 返回任务图的启动调度器。 */
  auto query(stdexec::get_start_scheduler_t) const noexcept -> execution_scheduler<Mode> {
    return scheduler;
  }

  /** @brief 返回任务图的运行时调度器。 */
  auto query(stdexec::get_scheduler_t) const noexcept -> execution_scheduler<Mode> {
    return scheduler;
  }

  /** @brief 返回根任务图的停止令牌。 */
  auto query(stdexec::get_stop_token_t) const noexcept -> root_stop_token {
    return root_stop_token{root->stop_token()};
  }

  /** @brief 返回当前环境持有的任务图租约。 */
  auto query(get_external_root_t) const noexcept -> root_handle { return root; }

  /** @brief 将未覆盖的环境查询转发到底层环境。 */
  template <class Query>
    requires(!std::same_as<Query, stdexec::get_start_scheduler_t>
             && !std::same_as<Query, stdexec::get_scheduler_t>
             && !std::same_as<Query, stdexec::get_stop_token_t>
             && !std::same_as<Query, get_external_root_t>
             && requires(Query q, const Base& b) { q(b); })
  decltype(auto) query(Query q) const noexcept(noexcept(q(base))) {
    return q(base);
  }
};

/** @brief 请求所属外部任务图协作停止。 */
struct request_root_stop {
  faio::detail::external_root_state*
      root{}; /**< 持有任务图的根状态租约，维持接纳、取消及排空的归属。 */

  /** @brief 向所属根任务图转发停止请求。 */
  void operator()() const noexcept { root->request_stop(); }
};
template <class Token, class Callback>
using stop_callback = stdexec::stop_callback_for_t<Token, Callback>;
}  // namespace faio::experimental::detail
#endif
