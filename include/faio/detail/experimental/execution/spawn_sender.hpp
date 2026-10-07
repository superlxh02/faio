#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SPAWN_SENDER_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SPAWN_SENDER_HPP
#include "faio/detail/experimental/execution/sender_join_handle.hpp"

namespace faio::experimental::detail {
template <class Sender, class Tuple>
struct submitted_operation final : graph_operation_storage {
  using operation_type = stdexec::connect_result_t<Sender, result_receiver<Tuple>>;
  operation_type operation;
  submitted_operation(Sender sender, submitted_graph<Tuple>& state)
      : operation(stdexec::connect(std::move(sender), result_receiver<Tuple>{&state})) {}
  void start() noexcept override { stdexec::start(operation); }
  auto root() const noexcept -> root_handle override { return operation.root(); }
};
} // namespace faio::experimental::detail

namespace faio::experimental {
template <class Sender> auto runtime_ref::spawn_sender(Sender&& sender) const {
  auto bound = bind(std::forward<Sender>(sender));
  using tuple = detail::result_tuple<decltype(bound)>;
  auto state = std::make_shared<detail::submitted_graph<tuple>>(context().external_host());
  state->operation = std::make_unique<detail::submitted_operation<decltype(bound), tuple>>(std::move(bound), *state);
  state->root = state->operation->root();
  if (faio::detail::current_external_host() == state->host) {
    state->parent_tracker = faio::detail::current_tracker;
    state->parent_stop = faio::detail::current_stop_token;
    state->parent_cancellation_owner = faio::detail::current_cancellation_owner;
    state->cancellation_policy = faio::detail::current_cancellation_policy;
    state->parent_link.emplace(state->parent_stop, detail::request_root_stop{state->root.get()});
  }
  state->root->on_drained(state->cleanup_node);
  state->execution_keepalive = state;
  if (state->parent_tracker) state->parent_tracker->add();
  // All allocations and stop registrations are complete before noexcept start.
  state->operation->start();
  return sender_join_handle<tuple>{std::move(state)};
}
} // namespace faio::experimental
#endif
