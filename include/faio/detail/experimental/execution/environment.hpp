#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_ENVIRONMENT_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_ENVIRONMENT_HPP
#include "faio/detail/experimental/execution/scheduler.hpp"

namespace faio::experimental::detail {
template <faio::runtime::mode Mode, class Base = stdexec::env<>>
struct root_environment {
  execution_scheduler<Mode> scheduler;
  root_handle root;
  Base base{};
  auto query(stdexec::get_start_scheduler_t) const noexcept -> execution_scheduler<Mode> { return scheduler; }
  auto query(stdexec::get_scheduler_t) const noexcept -> execution_scheduler<Mode> { return scheduler; }
  auto query(stdexec::get_stop_token_t) const noexcept -> root_stop_token { return root_stop_token{root->stop_token()}; }
  auto query(get_external_root_t) const noexcept -> root_handle { return root; }
  template <class Query>
    requires (!std::same_as<Query, stdexec::get_start_scheduler_t> &&
              !std::same_as<Query, stdexec::get_scheduler_t> &&
              !std::same_as<Query, stdexec::get_stop_token_t> &&
              !std::same_as<Query, get_external_root_t> &&
              requires(Query q, const Base& b) { q(b); })
  decltype(auto) query(Query q) const noexcept(noexcept(q(base))) { return q(base); }
};

struct request_root_stop {
  faio::detail::external_root_state* root{};
  void operator()() const noexcept { root->request_stop(); }
};
template <class Token, class Callback>
using stop_callback = stdexec::stop_callback_for_t<Token, Callback>;
} // namespace faio::experimental::detail
#endif
