// Core hooks are deliberately usable as C++23 without experimental macros.
#include <faio/detail/coroutine/join_handle.hpp>
#include <faio/detail/coroutine/this_coro.hpp>
#include <cassert>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace faio::detail;
using policy = cancellation_error_policy;
struct queue_scheduler {
  std::deque<std::coroutine_handle<>> ready;
  void enqueue(std::coroutine_handle<> handle) { ready.push_back(handle); }
  void drain() {
    int local{};
    faio::detail::execution_thread_binding binding{faio::scheduler_ref{*this}, {}, local, 0};
    faio::detail::execution_thread_guard execution{binding};
    while (!ready.empty()) {
      auto handle = ready.front();
      ready.pop_front();
      current_tracker = nullptr;
      current_stop_token = {};
      current_cancellation_policy = policy::fatal_if_unobserved;
      current_external_scope = {};
      handle.resume();
    }
    current_tracker = nullptr;
    current_stop_token = {};
    current_cancellation_policy = policy::fatal_if_unobserved;
    current_external_scope = {};
  }
};

faio::task<void> cancelled() { throw faio::operation_cancelled{}; co_return; }
faio::task<void> failed() { throw std::runtime_error("non-cancellation"); co_return; }
faio::task<void> finished() { co_return; }
faio::task<void> nested_context(std::stop_token expected, external_scope_ref scope) {
  assert((co_await faio::this_coro::stop_token()) == expected);
  assert(current_cancellation_policy == policy::normal_if_stop_requested);
  assert(current_external_scope.host == scope.host && current_external_scope.root_state == scope.root_state);
  co_await faio::this_coro::yield();
  assert((co_await faio::this_coro::stop_token()) == expected);
  assert(current_cancellation_policy == policy::normal_if_stop_requested);
  assert(current_external_scope.root_state == scope.root_state);
}
faio::task<void> parent_context(std::stop_token expected, external_scope_ref scope) {
  co_await nested_context(expected, scope);
}

template <class F>
void must_terminate(F action) {
  const auto child = ::fork();
  assert(child >= 0);
  if (child == 0) {
    std::set_terminate([] { std::_Exit(86); });
    action();
    std::_Exit(0);
  }
  int status{};
  assert(::waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 86);
}

void root_policy(bool requested, policy selected, bool non_cancel = false) {
  queue_scheduler scheduler;
  std::stop_source source;
  if (requested) source.request_stop();
  current_stop_token = source.get_token();
  current_cancellation_policy = selected;
  start_unobserved(faio::scheduler_ref{scheduler}, non_cancel ? failed() : cancelled());
  scheduler.drain();
}

struct completion_state {
  int stage{}, publishers{};
  void register_task() noexcept { stage = 1; }
  void finish_task() noexcept { assert(stage == 2 && publishers == 1); stage = 3; }
  static void begin(void* object) noexcept { ++static_cast<completion_state*>(object)->publishers; }
  static void zero(void* object) noexcept {
    auto& state = *static_cast<completion_state*>(object);
    assert(state.stage == 1 && state.publishers == 1);
    state.stage = 2;
  }
  static void end(void* object) noexcept {
    auto& state = *static_cast<completion_state*>(object);
    assert(state.stage == 3 && state.publishers == 1);
    --state.publishers;
    state.stage = 4;
  }
};
}

int main() {
  queue_scheduler scheduler;
  std::stop_source source;
  external_work_host host;
  auto lease = host.acquire_root();
  const external_scope_ref scope{&host, lease.get()};
  current_stop_token = source.get_token();
  current_cancellation_policy = policy::normal_if_stop_requested;
  current_external_scope = scope;
  task_tracker tracker;
  start_unobserved(faio::scheduler_ref{scheduler}, parent_context(source.get_token(), scope), &tracker);
  // Destroy caller TLS before first execution: the root must have captured it.
  current_stop_token = {};
  current_cancellation_policy = policy::fatal_if_unobserved;
  current_external_scope = {};
  scheduler.drain();
  assert(tracker.pending.load() == 0);
  lease->terminal();

  root_policy(true, policy::normal_if_stop_requested);
  must_terminate([] { root_policy(false, policy::normal_if_stop_requested); });
  must_terminate([] { root_policy(true, policy::fatal_if_unobserved); });
  must_terminate([] { root_policy(true, policy::normal_if_stop_requested, true); });

  source.request_stop();
  current_cancellation_policy = policy::normal_if_stop_requested;
  auto state = start_observed(faio::scheduler_ref{scheduler}, cancelled(), nullptr, source.get_token());
  state->abandon();
  scheduler.drain();
  assert(state->normal_cancelled && state->error);
  bool observed{};
  try { state->take(); } catch (const faio::operation_cancelled&) { observed = true; }
  assert(observed);
  must_terminate([] {
    queue_scheduler local;
    current_cancellation_policy = policy::normal_if_stop_requested;
    auto error = start_observed(faio::scheduler_ref{local}, failed(), nullptr, {});
    error->abandon();
    local.drain();
  });

  completion_state tail;
  task_tracker protected_tracker;
  protected_tracker.install_zero_completion_hook(&tail, completion_state::zero,
      completion_state::begin, completion_state::end);
  start_unobserved(faio::scheduler_ref{scheduler}, finished(), &protected_tracker,
                   faio::task_lifetime_ref{tail});
  scheduler.drain();
  assert(tail.stage == 4 && tail.publishers == 0);
}
