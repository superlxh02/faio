// Reviewable P0 alternative only. This deliberately changes the internal root
// token query's type from plan section 9.2; it is not production faio code.
#include <stdexec/execution.hpp>
#include <exec/task.hpp>

#include <concepts>
#include <cstdio>
#include <exception>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace {
class faio_probe_stop_token {
 public:
  template <class Callback>
  using callback_type = std::stop_callback<Callback>;

  faio_probe_stop_token() noexcept = default;
  explicit faio_probe_stop_token(std::stop_token token) noexcept : token_(std::move(token)) {}

  bool stop_requested() const noexcept { return token_.stop_requested(); }
  bool stop_possible() const noexcept { return token_.stop_possible(); }
  operator std::stop_token() const noexcept { return token_; }
  bool operator==(const faio_probe_stop_token&) const noexcept = default;

 private:
  std::stop_token token_;
};

struct wrapper_environment {
  faio_probe_stop_token token;

  auto query(stdexec::get_stop_token_t) const noexcept -> faio_probe_stop_token { return token; }
  auto query(stdexec::get_start_scheduler_t) const noexcept -> stdexec::inline_scheduler { return {}; }
  auto query(stdexec::get_scheduler_t) const noexcept -> stdexec::inline_scheduler { return {}; }
};

struct completion_state {
  int value{};
  unsigned value_count{};
  unsigned stopped_count{};
  std::exception_ptr error;
};

struct observing_receiver {
  using receiver_concept = stdexec::receiver_tag;
  wrapper_environment environment;
  completion_state* state;

  auto get_env() const noexcept -> wrapper_environment { return environment; }
  void set_value(int value) && noexcept {
    state->value = value;
    ++state->value_count;
  }
  void set_stopped() && noexcept { ++state->stopped_count; }
  void set_error(std::exception_ptr error) && noexcept { state->error = std::move(error); }
};

static_assert(stdexec::stoppable_token<faio_probe_stop_token>);
static_assert(!stdexec::unstoppable_token<faio_probe_stop_token>);
static_assert(std::is_nothrow_copy_constructible_v<faio_probe_stop_token>);
static_assert(std::is_nothrow_convertible_v<faio_probe_stop_token, std::stop_token>);
static_assert(std::same_as<stdexec::stop_callback_for_t<faio_probe_stop_token, void (*)()>,
                           std::stop_callback<void (*)()>>);

auto value_task() -> exec::task<int> {
  const auto token = co_await stdexec::get_stop_token();
  if (token.stop_requested())
    co_await stdexec::just_stopped();
  co_await stdexec::schedule(stdexec::inline_scheduler{});
  co_return 42;
}

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

template <bool StartsOn, class Sender>
auto run_probe(Sender sender, std::stop_token token) -> completion_state {
  completion_state state;
  const wrapper_environment environment{faio_probe_stop_token{std::move(token)}};
  if constexpr (StartsOn) {
    auto operation = stdexec::connect(stdexec::starts_on(stdexec::inline_scheduler{}, std::move(sender)),
                                      observing_receiver{environment, &state});
    stdexec::start(operation);
  } else {
    auto operation = stdexec::connect(std::move(sender), observing_receiver{environment, &state});
    stdexec::start(operation);
  }
  return state;
}

bool check(bool condition, const char* name, bool starts_on) {
  if (!condition)
    std::fprintf(stderr, "stop token wrapper alternative failed (%s): %s\n",
                  starts_on ? "starts_on" : "direct", name);
  return condition;
}

template <bool StartsOn>
bool verify_cases() {
  bool succeeded = true;
  std::stop_source active_source;
  const auto normal = run_probe<StartsOn>(value_task(), active_source.get_token());
  succeeded &= check(normal.value == 42 && normal.value_count == 1 && normal.stopped_count == 0 && !normal.error,
                     "unstopped task completes value once", StartsOn);

  std::stop_source stopped_source;
  stopped_source.request_stop();
  const auto stopped = run_probe<StartsOn>(value_task(), stopped_source.get_token());
  succeeded &= check(stopped.value_count == 0 && stopped.stopped_count == 1 && !stopped.error,
                     "pre-stopped root completes stopped once", StartsOn);

  bool initially_clear = false;
  bool observed_stop = false;
  const auto requested = run_probe<StartsOn>(stop_observing_task(active_source, initially_clear, observed_stop),
                                            active_source.get_token());
  succeeded &= check(initially_clear && observed_stop && requested.value_count == 0 &&
                         requested.stopped_count == 1 && !requested.error,
                     "std::stop_source request reaches task token and stopped completion", StartsOn);
  return succeeded;
}
}  // namespace

int main() {
  // && would skip the second alternative's cases if the direct cases fail.
  const bool direct_succeeded = verify_cases<false>();
  const bool starts_on_succeeded = verify_cases<true>();
  const bool succeeded = direct_succeeded && starts_on_succeeded;
  std::printf("stop token wrapper alternative P0: compiler=%s libstdc++=%d status=%s\n",
              __VERSION__, _GLIBCXX_RELEASE, succeeded ? "passed" : "failed");
  return succeeded ? 0 : 1;
}
