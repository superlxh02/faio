#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <exec/task.hpp>
#include <atomic>
#include <chrono>
#include <thread>

namespace ex = faio::experimental;
using namespace std::chrono_literals;
faio::task<int> cooperatively_cancelled(std::atomic<bool>& entered, std::atomic<int>& cleaned) {
  struct cleanup { std::atomic<int>& count; ~cleanup() { ++count; } } guard{cleaned};
  entered.store(true, std::memory_order_release); entered.notify_all();
  co_await faio::time::sleep(1h);
  co_return 42;
}
faio::task<int> successful_after_stop() { co_return 42; }
struct value_receiver {
  using receiver_concept = stdexec::receiver_tag;
  std::atomic<bool>* done;
  int* value;
  auto get_env() const noexcept { return stdexec::env<>{}; }
  void set_value(int result) && noexcept { *value = result; done->store(true, std::memory_order_release); }
  void set_error(std::exception_ptr) && noexcept { *value = -1; done->store(true, std::memory_order_release); }
  void set_stopped() && noexcept { *value = -2; done->store(true, std::memory_order_release); }
};
faio::task<int> unmatched_cancel() { throw faio::operation_cancelled{}; co_return 0; }
faio::task<void> descendant(std::atomic<bool>& finished) {
  co_await faio::time::sleep(2ms);
  finished.store(true, std::memory_order_release);
}
faio::task<int> parent(std::atomic<bool>& finished) {
  faio::spawn_detached(descendant(finished));
  co_return 42;
}
template <class Runtime> bool verify(Runtime& runtime) {
  std::atomic<bool> child_finished{false};
  const auto parent_result = runtime.run(runtime.as_sender(parent(child_finished)));
  if (!parent_result || std::get<0>(*parent_result) != 42 || !child_finished.load()) return false;
  bool unmatched_observed{};
  try { (void)runtime.run(runtime.as_sender(unmatched_cancel())); }
  catch (const faio::operation_cancelled&) { unmatched_observed = true; }
  if (!unmatched_observed) return false;
  std::atomic<bool> entered{false};
  std::atomic<int> cleaned{};
  std::jthread stopper([&] {
    while (!entered.load(std::memory_order_acquire)) entered.wait(false, std::memory_order_acquire);
    runtime.request_stop();
  });
  const auto stopped = runtime.run(runtime.as_sender(cooperatively_cancelled(entered, cleaned)));
  stopper.join();
  if (stopped || cleaned.load() != 1) return false;
  // Even with a requested root stop, a task which returns normally retains its
  // real value. schedule itself cooperates, so exercise as_sender directly.
  std::atomic<bool> normal_done{};
  int normal_value{};
  auto operation = stdexec::connect(runtime.as_sender(successful_after_stop()),
      value_receiver{&normal_done, &normal_value});
  stdexec::start(operation);
  runtime.ref().context().drive_until([&] {
    return normal_done.load(std::memory_order_acquire) && runtime.ref().context().external_host().quiescent();
  });
  if (normal_value != 42) return false;
  runtime.shutdown();
  return true;
}
int main() {
  ex::current_thread_runtime current;
  auto options = ex::multi_thread_defaults(); options.workers = 1;
  ex::multi_thread_runtime one{options};
  options.workers = 4;
  ex::multi_thread_runtime four{options};
  return verify(current) && verify(one) && verify(four) ? 0 : 1;
}
