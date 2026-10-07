#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <atomic>
#include <cstdlib>
#include <new>
#include <stop_token>
#include <type_traits>
#include <vector>

namespace ex = faio::experimental;
namespace {
thread_local bool reject_allocations{};
thread_local std::size_t failing_allocation{}, allocation_count{};
thread_local bool fail_queue_expansion{};
thread_local unsigned queue_expansion_failures{};
faio::task<int> immediate_task() { co_return 42; }
faio::task<int> cancellable_task() {
  co_await faio::time::sleep(std::chrono::hours{1});
  co_return 42;
}
struct stopping_env {
  std::stop_token token;
  auto query(stdexec::get_stop_token_t) const noexcept { return token; }
};
struct destroying_receiver {
  using receiver_concept = stdexec::receiver_tag;
  void* storage;
  void (*destroy)(void*) noexcept;
  std::atomic<int>* completions;
  std::stop_token token;
  auto get_env() const noexcept { return stopping_env{token}; }
  void complete(int result) noexcept {
    const auto cleanup = destroy;
    const auto pointer = storage;
    auto* count = completions;
    count->store(result, std::memory_order_release);
    cleanup(pointer);
  }
  template <class... Values> void set_value(Values&&...) && noexcept { complete(1); }
  void set_error(std::exception_ptr) && noexcept { complete(2); }
  void set_stopped() && noexcept { complete(-1); }
};
template <class Runtime>
bool verify(Runtime& runtime, bool stoppable) {
  auto scheduler = runtime.get_scheduler();
  using sender = decltype(stdexec::schedule(scheduler));
  static_assert(stdexec::scheduler<decltype(scheduler)>);
  static_assert(!std::default_initializable<decltype(scheduler)>);
  static_assert(std::same_as<stdexec::completion_signatures_of_t<sender, stdexec::env<>>,
      stdexec::completion_signatures<stdexec::set_value_t()>>);
  static_assert(std::same_as<stdexec::completion_signatures_of_t<sender, stopping_env>,
      stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>>);
  if (stdexec::get_completion_scheduler<stdexec::set_value_t>(scheduler) != scheduler) return false;
  for (int iteration = 0; iteration != 64; ++iteration) {
    using operation = stdexec::connect_result_t<sender, destroying_receiver>;
    operation* pointer{};
    std::atomic<int> completion{};
    std::stop_source source;
    if (stoppable) source.request_stop();
    pointer = new operation(stdexec::connect(stdexec::schedule(scheduler), destroying_receiver{
        &pointer, +[](void* storage) noexcept {
          auto** pointer = static_cast<operation**>(storage);
          delete std::exchange(*pointer, nullptr);
        }, &completion, source.get_token()}));
    reject_allocations = true;
    stdexec::start(*pointer); // allocation failure must not affect accepted work
    reject_allocations = false;
    runtime.ref().context().drive_until([&] {
      return completion.load(std::memory_order_acquire) != 0 && runtime.ref().context().external_host().quiescent();
    });
    if (pointer || completion.load() != (stoppable ? -1 : 1)) return false;
  }
  return true;
}
template <class Runtime> bool verify_connect_failures(Runtime& runtime) {
  unsigned failures{};
  std::stop_source source;
  std::atomic<int> completion{};
  for (std::size_t position = 1; position != 17; ++position) {
    auto sender = runtime.as_sender(immediate_task());
    allocation_count = 0;
    failing_allocation = position;
    try {
      auto operation = stdexec::connect(std::move(sender), destroying_receiver{
          nullptr, +[](void*) noexcept {}, &completion, source.get_token()});
    } catch (const std::bad_alloc&) { ++failures; }
    failing_allocation = 0;
    if (!runtime.ref().context().external_host().quiescent()) return false;
  }
  return failures >= 4 && completion.load() == 0;
}
template <class Runtime> bool verify_task_start(Runtime& runtime, bool cancelled) {
  for (int iteration = 0; iteration != 16; ++iteration) {
    auto sender = runtime.as_sender(cancelled ? cancellable_task() : immediate_task());
    using operation = stdexec::connect_result_t<decltype(sender), destroying_receiver>;
    operation* pointer{};
    std::atomic<int> completion{};
    std::stop_source source;
    if (cancelled) source.request_stop();
    pointer = new operation(stdexec::connect(std::move(sender), destroying_receiver{
        &pointer, +[](void* storage) noexcept {
          auto** pointer = static_cast<operation**>(storage);
          delete std::exchange(*pointer, nullptr);
        }, &completion, source.get_token()}));
    reject_allocations = true;
    stdexec::start(*pointer);
    reject_allocations = false;
    runtime.ref().context().drive_until([&] {
      return completion.load(std::memory_order_acquire) && runtime.ref().context().external_host().quiescent();
    });
    if (pointer || completion.load() != (cancelled ? -1 : 1)) return false;
  }
  return true;
}

struct frame_payload {
  std::atomic<unsigned>* destroyed;
  explicit frame_payload(std::atomic<unsigned>& count) noexcept : destroyed(&count) {}
  frame_payload(frame_payload&& other) noexcept : destroyed(std::exchange(other.destroyed, nullptr)) {}
  frame_payload(const frame_payload&) = delete;
  ~frame_payload() { if (destroyed) destroyed->fetch_add(1, std::memory_order_release); }
};
faio::task<int> must_not_start(frame_payload, std::atomic<unsigned>& started) {
  started.fetch_add(1, std::memory_order_relaxed);
  co_return 42;
}
faio::detail::detached_task queue_filler(std::atomic<unsigned>& completed) {
  completed.fetch_add(1, std::memory_order_release);
  co_return;
}
struct enqueue_error_receiver {
  using receiver_concept = stdexec::receiver_tag;
  unsigned* completions;
  bool* bad_allocation;
  auto get_env() const noexcept { return stdexec::env<>{}; }
  void set_value(int) && noexcept { ++*completions; }
  void set_stopped() && noexcept { ++*completions; }
  void set_error(std::exception_ptr error) && noexcept {
    ++*completions;
    try { std::rethrow_exception(error); }
    catch (const std::bad_alloc&) { *bad_allocation = true; }
    catch (...) {}
  }
};
bool verify_enqueue_failure() {
  // A fresh current-thread queue holds 64 handles before its first expansion.
  // Prebuild every frame and connect the bridge before enabling fault injection.
  ex::current_thread_runtime runtime;
  std::atomic<unsigned> completed{}, started{}, destroyed{};
  std::vector<faio::detail::detached_task> fillers;
  fillers.reserve(64);
  for (unsigned index = 0; index != 64; ++index)
    fillers.push_back(queue_filler(completed));
  unsigned completions{};
  bool bad_allocation{};
  auto operation = stdexec::connect(runtime.as_sender(must_not_start(frame_payload{destroyed}, started)),
                                   enqueue_error_receiver{&completions, &bad_allocation});
  queue_expansion_failures = 0;
  auto result = runtime.run(stdexec::schedule(runtime.get_scheduler()) | stdexec::then([&] {
    for (auto& frame : fillers)
      faio::detail::start_detached(std::move(frame), faio::detail::current_scheduler());
    stdexec::start(operation);

    // This is a dispatch-stage failure in the existing coroutine queue, not an
    // external start/publication failure. Execute its real queued start node
    // with an independent guard while all 64 filler frames remain queued: the
    // ordinary driver would otherwise resume one filler before this next node.
    // No production hook or artificial scheduler replaces the real runtime.
    auto dispatch = runtime.ref().context().external_host().try_pop_external(0);
    if (!dispatch) std::terminate();
    faio::detail::cooperative_poll_scope budget;
    faio::detail::scoped_task_context context{
        {.stop_token = dispatch.root()->stop_token(), .external_scope = dispatch.scope()}};
    fail_queue_expansion = true;
    dispatch.node()->execute(dispatch.node());
    fail_queue_expansion = false;
  }));
  runtime.ref().context().wait_all();
  // Delivery requires the bridge tracker to reach zero. Quiescence and the
  // subsequent successful run/shutdown also cover root, reservation and runtime
  // lifetime rollback after destruction of the unsubmitted wrapper and child.
  const bool rolled_back = result && completions == 1 && bad_allocation
      && queue_expansion_failures == 1 && completed.load(std::memory_order_acquire) == 64
      && started.load(std::memory_order_acquire) == 0 && destroyed.load(std::memory_order_acquire) == 1
      && runtime.ref().context().external_host().quiescent();
  auto next = runtime.run(runtime.as_sender(immediate_task()));
  runtime.shutdown();
  return rolled_back && next && std::get<0>(*next) == 42;
}
} // namespace
[[gnu::noinline]] void* operator new(std::size_t size) {
  if (fail_queue_expansion && size == 128 * sizeof(std::coroutine_handle<>)) {
    fail_queue_expansion = false;
    ++queue_expansion_failures;
    throw std::bad_alloc{};
  }
  if (reject_allocations || (failing_allocation && ++allocation_count == failing_allocation)) throw std::bad_alloc{};
  if (auto* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc{};
}
[[gnu::noinline]] void operator delete(void* memory) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
  ex::current_thread_runtime current;
  auto options = ex::multi_thread_defaults(); options.workers = 1;
  ex::multi_thread_runtime one{options};
  options.workers = 4;
  ex::multi_thread_runtime multi{options};
  return verify(current, false) && verify(current, true) && verify(one, false) && verify(one, true)
      && verify(multi, false) && verify(multi, true) && verify_connect_failures(current)
      && verify_connect_failures(one) && verify_connect_failures(multi)
      && verify_task_start(current, false) && verify_task_start(current, true)
      && verify_task_start(one, false) && verify_task_start(one, true)
      && verify_task_start(multi, false) && verify_task_start(multi, true)
      && verify_enqueue_failure() ? 0 : 1;
}
