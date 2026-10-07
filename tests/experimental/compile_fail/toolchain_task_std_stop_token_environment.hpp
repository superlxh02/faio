#ifndef FAIO_TEST_TASK_STD_STOP_TOKEN_ENVIRONMENT_HPP
#define FAIO_TEST_TASK_STD_STOP_TOKEN_ENVIRONMENT_HPP

// Minimal upstream API reproduction. The receiver uses the exact root
// get_stop_token/get_start_scheduler contract required by plan section 9.2.
#include <stdexec/execution.hpp>
#include <exec/task.hpp>
#include <exception>
#include <stop_token>

struct std_stop_token_environment {
  std::stop_token token;

  auto query(stdexec::get_stop_token_t) const noexcept -> std::stop_token { return token; }
  auto query(stdexec::get_start_scheduler_t) const noexcept -> stdexec::inline_scheduler { return {}; }
  auto query(stdexec::get_scheduler_t) const noexcept -> stdexec::inline_scheduler { return {}; }
};

struct std_stop_token_receiver {
  using receiver_concept = stdexec::receiver_tag;
  std_stop_token_environment environment;

  auto get_env() const noexcept -> std_stop_token_environment { return environment; }
  void set_value(int) && noexcept {}
  void set_stopped() && noexcept {}
  void set_error(std::exception_ptr) && noexcept {}
};

inline auto std_stop_token_task() -> exec::task<int> {
  co_await stdexec::schedule(stdexec::inline_scheduler{});
  co_return 42;
}

#endif
