/**
 * @file faio/detail/experimental/execution/spawn_sender.hpp
 * @brief 发送器派生任务。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SPAWN_SENDER_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SPAWN_SENDER_HPP
#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/experimental/execution/sender_join_handle.hpp"

namespace faio::experimental::detail {
/** @brief 保存连接已提交 sender 与结果接收器的操作状态。 */
template <class Sender, class Tuple>
struct submitted_operation final : graph_operation_storage {
  using operation_type = stdexec::connect_result_t<
      Sender,
      result_receiver<Tuple>>; /**< 连接发送器与接收器后得到的稳定操作状态类型。 */
  operation_type operation;    /**< 地址固定的底层异步操作，由当前包装负责启动、等待和交付结果。 */

  /** @brief 将 sender 连接到提交结果的接收器。 */
  submitted_operation(Sender sender, submitted_graph<Tuple>& state)
      : operation(stdexec::connect(std::move(sender), result_receiver<Tuple>{&state})) {}

  /** @brief 按执行协议启动已连接的操作，此后只能交付一次终态。 */
  void start() noexcept override { stdexec::start(operation); }

  /** @brief 返回关联任务图的根状态租约。 */
  auto root() const noexcept -> root_handle override { return operation.root(); }
};
}  // namespace faio::experimental::detail

namespace faio::experimental {
/** @brief 立即提交 sender，并返回一次性结果观察句柄。 */
template <class Sender>
auto runtime_ref::spawn_sender(Sender&& sender) const {
  auto bound = bind(std::forward<Sender>(sender));
  using tuple = detail::result_tuple<decltype(bound)>;
  auto state = std::make_shared<detail::submitted_graph<tuple>>(context().external_host());
  state->operation = std::make_unique<detail::submitted_operation<decltype(bound), tuple>>(
      std::move(bound), *state);
  state->root = state->operation->root();
  if (faio::detail::current_external_host() == state->host) {
    state->parent_tracker = ::faio::detail::current_task_context().scope;
    state->parent_stop = ::faio::detail::current_task_context().stop_token;
    state->parent_cancellation_owner = ::faio::detail::current_task_context().cancellation_owner;
    state->cancellation_policy = ::faio::detail::current_task_context().cancellation_policy;
    state->parent_link.emplace(state->parent_stop, detail::request_root_stop{state->root.get()});
  }
  state->root->on_drained(state->cleanup_node);
  state->execution_keepalive = state;
  if (state->parent_tracker)
    state->parent_tracker->add();
  state->operation->start();
  return sender_join_handle<tuple>{std::move(state)};
}
}  // namespace faio::experimental
#endif
