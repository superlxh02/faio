// Additional P0 evidence: public when_all adapts the root stop protocol for
// exec::task. Direct and starts_on-only failures remain as separate probes.
#include "compile_fail/toolchain_task_std_stop_token_environment.hpp"

#include <cstdio>
#include <utility>

namespace {
struct completion_state {
  int value{};
  unsigned value_count{};
  unsigned stopped_count{};
  std::exception_ptr error;
};

struct observing_receiver {
  using receiver_concept = stdexec::receiver_tag;
  std_stop_token_environment environment;
  completion_state* state;

  auto get_env() const noexcept -> std_stop_token_environment { return environment; }
  void set_value(int value) && noexcept {
    state->value = value;
    ++state->value_count;
  }
  void set_stopped() && noexcept { ++state->stopped_count; }
  void set_error(std::exception_ptr error) && noexcept { state->error = std::move(error); }
};

auto stop_observing_task(std::stop_source& source, bool& initially_clear, bool& observed_stop)
    -> exec::task<int> {
  const auto token = co_await stdexec::get_stop_token();
  initially_clear = !token.stop_requested();
  source.request_stop();
  observed_stop = token.stop_requested();
  if (observed_stop)
    co_await stdexec::just_stopped();
  co_return 42;
}

template <class Sender>
auto run_probe(Sender sender, std::stop_token token) -> completion_state {
  completion_state state;
  auto operation = stdexec::connect(
      stdexec::starts_on(stdexec::inline_scheduler{}, stdexec::when_all(std::move(sender))),
      observing_receiver{{token}, &state});
  stdexec::start(operation);
  return state;
}

bool check(bool condition, const char* name) {
  if (!condition)
    std::fprintf(stderr, "when_all stop protocol probe failed: %s\n", name);
  return condition;
}
}  // namespace

int main() {
  bool succeeded = true;
  std::stop_source active_source;
  const auto normal = run_probe(std_stop_token_task(), active_source.get_token());
  succeeded &= check(normal.value == 42 && normal.value_count == 1 && normal.stopped_count == 0 && !normal.error,
                     "unstopped task completes value once");

  std::stop_source stopped_source;
  stopped_source.request_stop();
  const auto stopped = run_probe(std_stop_token_task(), stopped_source.get_token());
  succeeded &= check(stopped.value_count == 0 && stopped.stopped_count == 1 && !stopped.error,
                     "pre-stopped root completes stopped once");

  bool initially_clear = false;
  bool observed_stop = false;
  const auto requested = run_probe(stop_observing_task(active_source, initially_clear, observed_stop),
                                   active_source.get_token());
  succeeded &= check(initially_clear && observed_stop && requested.value_count == 0 &&
                         requested.stopped_count == 1 && !requested.error,
                     "std::stop_source request reaches exec::task token and stopped completion");

  std::printf("when_all stop protocol P0: compiler=%s libstdc++=%d status=%s\n", __VERSION__, _GLIBCXX_RELEASE,
              succeeded ? "passed" : "failed");
  return succeeded ? 0 : 1;
}
