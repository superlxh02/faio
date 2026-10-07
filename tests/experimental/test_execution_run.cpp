#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <exec/task.hpp>
#include "multiple_value_sender.hpp"
#include <chrono>
#include <atomic>
#include <cstdio>
#include <expected>
#include <memory>
#include <stdexcept>
#include <thread>

namespace ex = faio::experimental;
using namespace std::chrono_literals;

faio::task<int> delayed_value() { co_await faio::time::sleep(2ms); co_return 42; }
faio::task<void> delayed_void() { co_await faio::time::sleep(1ms); }
faio::task<std::unique_ptr<int>> owned_value() { co_return std::make_unique<int>(42); }
faio::task<std::expected<int, int>> expected_error() { co_return std::unexpected(7); }
faio::task<int> failed_task() { throw std::runtime_error("bridge failure"); co_return 0; }
struct throwing_value {
  throwing_value() = default;
  throwing_value(throwing_value&&) { throw std::runtime_error("result move failure"); }
};
template <class Error> struct error_sender {
  using sender_concept = stdexec::sender_tag;
  Error error;
  template <class Self, class Environment = stdexec::env<>>
  static consteval auto get_completion_signatures() noexcept {
    return stdexec::completion_signatures<stdexec::set_value_t(int), stdexec::set_error_t(Error)>{};
  }
  template <class Receiver> struct operation {
    using operation_state_concept = stdexec::operation_state_tag;
    Receiver receiver;
    Error error;
    void start() noexcept { stdexec::set_error(std::move(receiver), std::move(error)); }
  };
  template <stdexec::receiver Receiver> auto connect(Receiver receiver) && -> operation<Receiver> {
    return {std::move(receiver), std::move(error)};
  }
};
faio::task<int> concurrent_job(std::atomic<unsigned>* armed, std::atomic<bool>* stop_sent, bool cancellable) {
  armed->fetch_add(1, std::memory_order_release);
  armed->notify_all();
  if (cancellable) co_await faio::time::sleep(1h);
  else {
    while (!stop_sent->load(std::memory_order_acquire)) co_await faio::this_coro::yield();
    co_await faio::time::sleep(1ms);
    if ((co_await faio::this_coro::stop_token()).stop_requested())
      throw std::runtime_error("one graph stopped another graph");
  }
  co_return 42;
}
template <class Runtime> bool verify_concurrent_roots(Runtime& runtime) {
  for (unsigned iteration = 0; iteration != 8; ++iteration) {
    std::atomic<unsigned> armed{};
    std::atomic<bool> stop_sent{};
    std::stop_source source;
    std::optional<std::tuple<int>> stopped, value;
    std::exception_ptr errors[2];
    std::thread first{[&] {
      try {
        stopped = runtime.run(stdexec::write_env(runtime.as_sender(concurrent_job(&armed, &stop_sent, true)),
            stdexec::prop(stdexec::get_stop_token, source.get_token())));
      } catch (...) { errors[0] = std::current_exception(); }
    }};
    std::thread second{[&] {
      try { value = runtime.run(runtime.as_sender(concurrent_job(&armed, &stop_sent, false))); }
      catch (...) { errors[1] = std::current_exception(); }
    }};
    for (auto count = armed.load(std::memory_order_acquire); count != 2;
         count = armed.load(std::memory_order_acquire)) armed.wait(count, std::memory_order_acquire);
    source.request_stop();
    stop_sent.store(true, std::memory_order_release);
    first.join(); second.join();
    if (errors[0] || errors[1] || stopped || !value || std::get<0>(*value) != 42) return false;
  }
  return true;
}
struct paused_receiver {
  using receiver_concept = stdexec::receiver_tag;
  std::atomic<bool>* entered;
  std::atomic<bool>* release;
  std::atomic<int>* result;
  faio::detail::external_work_host* host;
  auto get_env() const noexcept { return stdexec::env<>{}; }
  void complete(int value) noexcept {
    entered->store(true, std::memory_order_release);
    entered->notify_all();
    host->notify_completion();
    release->wait(false, std::memory_order_acquire);
    result->store(value, std::memory_order_release);
    host->notify_completion();
  }
  void set_value(int value) && noexcept { complete(value); }
  void set_stopped() && noexcept { complete(-1); }
  void set_error(std::exception_ptr) && noexcept { complete(-2); }
};
template <class Runtime> bool verify_foreign_completion(Runtime& runtime) {
  auto options = ex::multi_thread_defaults(); options.workers = 1;
  ex::multi_thread_runtime foreign{options};
  std::atomic<bool> entered{}, release{};
  std::atomic<int> result{};
  auto operation = stdexec::connect(runtime.bind(stdexec::continues_on(stdexec::just(42), foreign.get_scheduler())),
      paused_receiver{&entered, &release, &result, &runtime.ref().context().external_host()});
  auto root = operation.root();
  stdexec::start(operation);
  runtime.ref().context().drive_until([&] { return entered.load(std::memory_order_acquire); });
  // The graph has delivered its terminal marker from another host, but this
  // receiver is still using its result storage. Metadata alone cannot pin it.
  const bool pinned = !root->drained() && !runtime.ref().context().external_host().quiescent();
  release.store(true, std::memory_order_release); release.notify_all();
  runtime.ref().context().drive_until([&] {
    return result.load(std::memory_order_acquire) != 0 && root->drained();
  });
  foreign.shutdown();
  return pinned && result.load() == 42;
}
exec::task<int> external_workflow(ex::runtime_ref runtime) {
  auto answer = co_await runtime.as_sender(delayed_value());
  co_await runtime.as_sender(delayed_void());
  co_return answer;
}
template <class Runtime> bool verify(Runtime& runtime) {
  auto first = runtime.run(stdexec::then(stdexec::schedule(runtime.get_scheduler()), [] { return 42; }));
  if (!first || std::get<0>(*first) != 42) return false;
  int referenced = 42;
  auto copied = runtime.run(stdexec::just(std::ref(referenced)) |
      stdexec::then([](auto reference) -> int& { return reference.get(); }));
  static_assert(std::same_as<decltype(copied), std::optional<std::tuple<int>>>);
  referenced = 0;
  if (!copied || std::get<0>(*copied) != 42) return false;
  bool code_observed{}, payload_observed{};
  const auto code = std::make_error_code(std::errc::permission_denied);
  try { (void)runtime.run(error_sender<std::error_code>{code}); }
  catch (const std::system_error& error) { code_observed = error.code() == code; }
  try { (void)runtime.run(error_sender<int>{17}); }
  catch (const ex::execution_error<int>& error) { payload_observed = error.error() == 17; }
  if (!code_observed || !payload_observed) return false;
  auto normalized = runtime.run(stdexec::into_variant(multiple_value_sender{}));
  if (!normalized || std::get<0>(std::get<std::tuple<int>>(std::get<0>(*normalized))) != 42) return false;
  bool result_failure_observed{};
  try { (void)runtime.run(stdexec::then(stdexec::just(), [] { return throwing_value{}; })); }
  catch (const std::runtime_error& error) {
    result_failure_observed = std::string_view(error.what()) == "result move failure";
  }
  if (!result_failure_observed) return false;
  auto task = runtime.run(external_workflow(runtime.ref()));
  if (!task || std::get<0>(*task) != 42) return false;
  auto owned = runtime.run(runtime.as_sender(owned_value()));
  if (!owned || *std::get<0>(*owned) != 42) return false;
  auto expected = runtime.run(runtime.as_sender(expected_error()));
  if (!expected || std::get<0>(*expected).error() != 7) return false;
  bool threw = false;
  try { (void)runtime.run(runtime.as_sender(failed_task())); }
  catch (const std::runtime_error& error) { threw = std::string_view(error.what()) == "bridge failure"; }
  if (!threw) return false;
  auto combined = runtime.run(stdexec::continues_on(stdexec::when_all(
      runtime.as_sender(delayed_value()), stdexec::just(21)), runtime.get_scheduler()));
  if (!combined || std::get<0>(*combined) != 42 || std::get<1>(*combined) != 21) return false;
  runtime.shutdown();
  runtime.shutdown();
  return true;
}
int main() {
  ex::current_thread_runtime current;
  auto options = ex::multi_thread_defaults();
  options.workers = 2;
  ex::multi_thread_runtime multi{options};
  auto crossed = current.run(stdexec::continues_on(stdexec::just(42), multi.get_scheduler()));
  if (!crossed || std::get<0>(*crossed) != 42) return 1;
  auto bare = stdexec::sync_wait(stdexec::then(stdexec::schedule(multi.get_scheduler()), [] { return 42; }));
  if (!bare || std::get<0>(*bare) != 42) return 1;
  if (current.ref().context().block_on(delayed_value()) != 42) return 1;
  if (!verify_foreign_completion(current) || !verify_foreign_completion(multi)
      || !verify_concurrent_roots(current) || !verify_concurrent_roots(multi)) return 1;
  if (!verify(current) || !verify(multi)) return 1;
  std::puts("execution run/task bridge: passed");
}
