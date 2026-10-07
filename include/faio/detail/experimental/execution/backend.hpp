#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_BACKEND_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_BACKEND_HPP
#include "faio/detail/experimental/platform.hpp"
#if !defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) || !FAIO_EXPERIMENTAL_HAS_EXECUTION
#error "faio experimental: execution capability is not enabled"
#endif
#include <stdexec/execution.hpp>
#include <stop_token>
#include <utility>

namespace faio::experimental::detail {
// Approved adjustment to plan 9.2: exec::task in the locked backend requires
// callback_type. Ownership and cancellation remain in the standard stop state.
class root_stop_token {
 public:
  template <class Callback> using callback_type = std::stop_callback<Callback>;
  root_stop_token() noexcept = default;
  explicit root_stop_token(std::stop_token token) noexcept : token_(std::move(token)) {}
  bool stop_requested() const noexcept { return token_.stop_requested(); }
  bool stop_possible() const noexcept { return token_.stop_possible(); }
  operator std::stop_token() const noexcept { return token_; }
  bool operator==(const root_stop_token&) const noexcept = default;
 private:
  std::stop_token token_;
};
static_assert(stdexec::stoppable_token<root_stop_token>);
} // namespace faio::experimental::detail
#endif
