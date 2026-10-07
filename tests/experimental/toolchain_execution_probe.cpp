// P0 protocol probe for the locked NVIDIA/stdexec checkout. This intentionally
// uses an inline test scheduler; the faio adapter must enqueue to its host.
#if !defined(__linux__) || !defined(__GNUC__) || defined(__clang__) || \
    __GNUC__ != 16 || __GNUC_MINOR__ != 1
#error "faio experimental P0 execution probe requires Linux and GCC 16.1.x"
#endif

#include <stdexec/execution.hpp>
#include <exec/task.hpp>

#include <concepts>
#include <cstdio>
#include <exception>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {
namespace execution = stdexec;

class probe_scheduler;

template <execution::receiver Receiver>
class probe_operation {
 public:
  using operation_state_concept = execution::operation_state_tag;

  explicit probe_operation(Receiver receiver) noexcept(std::is_nothrow_move_constructible_v<Receiver>)
      : receiver_(std::move(receiver)) {}
  probe_operation(const probe_operation&) = delete;
  probe_operation& operator=(const probe_operation&) = delete;
  probe_operation(probe_operation&&) = delete;
  probe_operation& operator=(probe_operation&&) = delete;

  void start() noexcept {
    const auto token = execution::get_stop_token(execution::get_env(receiver_));
    // Preserve the same environment-dependent terminal channels as the probe's
    // get_completion_signatures. An unstoppable receiver only receives value.
    if constexpr (!execution::unstoppable_token<decltype(token)>) {
      if (token.stop_requested()) {
        execution::set_stopped(std::move(receiver_));
        return;
      }
    }
    execution::set_value(std::move(receiver_));
  }

 private:
  Receiver receiver_;
};

class probe_sender {
 public:
  using sender_concept = execution::sender_tag;

  explicit probe_sender(const int* resource) noexcept : resource_(resource) {}

  template <class Self, class Environment = execution::env<>>
  static consteval auto get_completion_signatures() noexcept {
    static_assert(std::same_as<std::remove_cvref_t<Self>, probe_sender>);
    using token = execution::stop_token_of_t<Environment>;
    if constexpr (execution::unstoppable_token<token>) {
      return execution::completion_signatures<execution::set_value_t()>{};
    } else {
      return execution::completion_signatures<execution::set_value_t(),
                                              execution::set_stopped_t()>{};
    }
  }

  template <execution::receiver Receiver>
  auto connect(Receiver receiver) const -> probe_operation<Receiver> {
    return probe_operation<Receiver>{std::move(receiver)};
  }

  auto get_env() const noexcept -> probe_scheduler;

 private:
  const int* resource_;
};

class probe_scheduler {
 public:
  using scheduler_concept = execution::scheduler_tag;

  explicit probe_scheduler(const int& resource) noexcept : resource_(&resource) {}

  auto schedule() const noexcept -> probe_sender { return probe_sender{resource_}; }

  template <class Completion>
    requires std::same_as<Completion, execution::set_value_t> ||
             std::same_as<Completion, execution::set_stopped_t>
  auto query(execution::get_completion_scheduler_t<Completion>) const noexcept
      -> probe_scheduler {
    return *this;
  }

  template <class Completion>
    requires std::same_as<Completion, execution::set_value_t> ||
             std::same_as<Completion, execution::set_stopped_t>
  auto query(execution::get_completion_domain_t<Completion>) const noexcept
      -> execution::default_domain {
    return {};
  }

  auto query(execution::get_domain_t) const noexcept -> execution::default_domain { return {}; }

  auto query(execution::get_forward_progress_guarantee_t) const noexcept
      -> execution::forward_progress_guarantee {
    return execution::forward_progress_guarantee::parallel;
  }

  bool operator==(const probe_scheduler&) const noexcept = default;

 private:
  const int* resource_;
};

auto probe_sender::get_env() const noexcept -> probe_scheduler {
  return probe_scheduler{*resource_};
}

struct never_stop_environment {
  auto query(execution::get_stop_token_t) const noexcept -> execution::never_stop_token {
    return {};
  }
};

struct stoppable_environment {
  std::stop_token token;
  probe_scheduler scheduler;

  auto query(execution::get_stop_token_t) const noexcept -> std::stop_token { return token; }
  auto query(execution::get_start_scheduler_t) const noexcept -> probe_scheduler { return scheduler; }
  auto query(execution::get_scheduler_t) const noexcept -> probe_scheduler { return scheduler; }
};

struct probe_receiver {
  using receiver_concept = execution::receiver_tag;
  stoppable_environment environment;
  int* completion;
  std::exception_ptr* error;

  auto get_env() const noexcept -> stoppable_environment { return environment; }
  void set_value() && noexcept { *completion = 1; }
  void set_value(int value) && noexcept { *completion = value; }
  void set_stopped() && noexcept { *completion = -1; }
  void set_error(std::exception_ptr value) && noexcept { *error = std::move(value); }
};

static_assert(execution::scheduler<probe_scheduler>);
static_assert(!std::default_initializable<probe_scheduler>);
static_assert(std::is_nothrow_copy_constructible_v<probe_scheduler>);
static_assert(std::is_nothrow_move_constructible_v<probe_scheduler>);
static_assert(execution::stoppable_token<std::stop_token>);
static_assert(execution::unstoppable_token<execution::never_stop_token>);
static_assert(std::same_as<execution::stop_callback_for_t<std::stop_token, void (*)()>,
                           std::stop_callback<void (*)()>>);
static_assert(std::same_as<execution::completion_signatures_of_t<probe_sender, execution::env<>>,
                           execution::completion_signatures<execution::set_value_t()>>);
static_assert(std::same_as<execution::completion_signatures_of_t<probe_sender, never_stop_environment>,
                           execution::completion_signatures<execution::set_value_t()>>);
static_assert(std::same_as<execution::completion_signatures_of_t<probe_sender, stoppable_environment>,
                           execution::completion_signatures<execution::set_value_t(),
                                                            execution::set_stopped_t()>>);

auto external_task(probe_scheduler scheduler) -> exec::task<int> {
  co_await execution::schedule(scheduler);
  const auto value = co_await execution::then(execution::just(20), [](int n) noexcept { return n + 1; });
  co_return value * 2;
}

bool check(bool condition, const char* name) {
  if (!condition)
    std::fprintf(stderr, "execution P0 probe failed: %s\n", name);
  return condition;
}
}  // namespace

int main() {
  const int resource{};
  const probe_scheduler scheduler{resource};
  bool succeeded = true;

  succeeded &= check(execution::get_completion_scheduler<execution::set_value_t>(
                         execution::get_env(execution::schedule(scheduler))) == scheduler,
                     "completion scheduler identity");
  succeeded &= check(execution::get_completion_scheduler<execution::set_stopped_t>(scheduler) == scheduler,
                     "stopped completion scheduler identity");

  auto task_result = execution::sync_wait(execution::starts_on(scheduler, external_task(scheduler)));
  succeeded &= check(task_result && std::get<0>(*task_result) == 42, "exec::task, starts_on, then, sync_wait");

  auto joined_result = execution::sync_wait(execution::continues_on(
      execution::when_all(
          execution::starts_on(scheduler, execution::just(19)),
          execution::then(execution::schedule(scheduler), []() noexcept { return 23; })),
      scheduler));
  succeeded &= check(joined_result && std::get<0>(*joined_result) + std::get<1>(*joined_result) == 42,
                     "when_all, continues_on");

  std::stop_source source;
  std::exception_ptr error;
  int completion{};
  auto operation = execution::connect(
      execution::schedule(scheduler), probe_receiver{{source.get_token(), scheduler}, &completion, &error});
  execution::start(operation);
  succeeded &= check(completion == 1 && !error, "schedule with std::stop_token");

  source.request_stop();
  completion = 0;
  auto stopped_operation = execution::connect(
      execution::schedule(scheduler), probe_receiver{{source.get_token(), scheduler}, &completion, &error});
  execution::start(stopped_operation);
  succeeded &= check(completion == -1 && !error, "stopped schedule delivery");

  std::printf("execution P0: compiler=%s libstdc++=%d status=%s\n", __VERSION__, _GLIBCXX_RELEASE,
              succeeded ? "passed" : "failed");
  return succeeded ? 0 : 1;
}
