#include "faio/experimental/execution.h"
#include "faio/faio.hpp"
#include <exec/task.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
struct empty_receiver {
  using receiver_concept = stdexec::receiver_tag;
  unsigned* completions;
  auto get_env() const noexcept { return stdexec::env<>{}; }
  void set_value() && noexcept { ++*completions; }
  void set_stopped() && noexcept { ++*completions; }
};

template <class Runtime>
void reservation_shutdown() {
  Runtime runtime;
  unsigned completions{};
  {
    auto operation = stdexec::connect(stdexec::schedule(runtime.get_scheduler()), empty_receiver{&completions});
    bool refused{};
    try { runtime.shutdown(); }
    catch (const std::logic_error&) { refused = true; }
    require(refused, "shutdown accepted unstarted top-level schedule operation");
    auto result = runtime.run(stdexec::just(21));
    require(result && std::get<0>(*result) == 21, "rejected shutdown changed accepting state");
    (void)operation;
  }
  require(completions == 0, "unstarted operation delivered a receiver");
  auto result = runtime.run(stdexec::just(42));
  require(result && std::get<0>(*result) == 42, "reservation destructor did not roll back");
  runtime.shutdown();
  runtime.shutdown();
}

template <class Runtime, class Scheduler>
faio::task<int> capture_scheduler(Runtime* runtime, std::unique_ptr<Scheduler>* saved) {
  *saved = std::make_unique<Scheduler>(runtime->get_scheduler());
  co_return 77;
}
template <class Runtime>
void metadata_shutdown() {
  using scheduler = decltype(std::declval<Runtime&>().get_scheduler());
  std::unique_ptr<scheduler> saved;
  {
    Runtime runtime;
    auto result = runtime.run(runtime.as_sender(capture_scheduler(&runtime, &saved)));
    require(result && std::get<0>(*result) == 77 && saved, "root-bound scheduler was not captured");
    require(saved->root() && !saved->root()->valid(), "completed graph metadata remains logically active");
    runtime.shutdown();
  }
  // 最后一个 scheduler metadata 副本析构不访问已经销毁的 host/context。
  saved.reset();
}

template <class Runtime>
faio::task<int> worker_rejection(Runtime* runtime) {
  int rejected{};
  try { (void)runtime->run(stdexec::just(1)); }
  catch (const std::logic_error&) { ++rejected; }
  try { runtime->shutdown(); }
  catch (const std::logic_error&) { ++rejected; }
  co_return rejected;
}
template <class Runtime>
void worker_shutdown() {
  Runtime runtime;
  auto result = runtime.run(runtime.as_sender(worker_rejection(&runtime)));
  require(result && std::get<0>(*result) == 2, "worker run/shutdown did not both reject");
  runtime.shutdown();
}

faio::task<int> pending_timer(std::atomic<bool>* armed) {
  armed->store(true, std::memory_order_release);
  armed->notify_all();
  co_await faio::time::sleep(std::chrono::seconds{10});
  co_return 0;
}
template <class Runtime>
exec::task<int> continue_after_quiescing(Runtime* runtime, std::atomic<bool>* armed,
                                         std::atomic<unsigned>* continued) {
  const auto value = co_await stdexec::upon_stopped(runtime->as_sender(pending_timer(armed)), [] { return 123; });
  auto scheduler = runtime->get_scheduler();
  continued->fetch_add(1, std::memory_order_release);
  // 此合法已接受 graph 的续体在关闭中仍可 connect/start。禁用本次 schedule 的
  // stop token，以同时验证不可停止 completion 在宿主停止时仍交付 value。
  co_await stdexec::write_env(stdexec::schedule(scheduler),
                            stdexec::prop(stdexec::get_stop_token, stdexec::never_stop_token{}));
  continued->fetch_add(1, std::memory_order_release);
  co_return value;
}
template <class Runtime>
void quiescing_continuation() {
  Runtime runtime;
  std::atomic<bool> armed{};
  std::atomic<unsigned> continued{};
  std::exception_ptr shutdown_error;
  std::thread closer{[&] {
    armed.wait(false, std::memory_order_acquire);
    try { runtime.shutdown(); }
    catch (...) { shutdown_error = std::current_exception(); }
  }};
  std::optional<std::tuple<int>> result;
  std::exception_ptr run_error;
  try { result = runtime.run(continue_after_quiescing(&runtime, &armed, &continued)); }
  catch (...) { run_error = std::current_exception(); }
  closer.join();
  if (shutdown_error) std::rethrow_exception(shutdown_error);
  if (run_error) std::rethrow_exception(run_error);
  require(result && std::get<0>(*result) == 123, "quiescing continuation lost value");
  require(continued.load(std::memory_order_acquire) == 2, "quiescing continuation was not dispatched");
}

faio::task<void> untracked_descendant(std::atomic<unsigned>* completed,
                                     std::atomic<unsigned>* valid_scopes) {
  co_await faio::time::sleep(std::chrono::milliseconds{3});
  if (faio::detail::current_external_scope.valid())
    valid_scopes->fetch_add(1, std::memory_order_relaxed);
  completed->fetch_add(1, std::memory_order_release);
}
template <class Runtime>
void untracked_native_descendants() {
  Runtime runtime;
  std::atomic<unsigned> completed{}, valid_scopes{};
  std::shared_ptr<faio::detail::external_root_state> metadata;
  auto result = runtime.run(stdexec::then(stdexec::schedule(runtime.get_scheduler()), [&] {
    require(faio::detail::current_tracker == nullptr, "schedule callback unexpectedly has a native tracker");
    metadata = faio::detail::current_external_scope.root_state->shared_from_this();
    faio::spawn_detached(untracked_descendant(&completed, &valid_scopes));
    (void)faio::spawn(untracked_descendant(&completed, &valid_scopes));
    return 41;
  }));
  const auto finished_before_return = completed.load(std::memory_order_acquire);
  runtime.shutdown(faio::io::shutdown_policy::drain);
  require(result && std::get<0>(*result) == 41, "native descendants changed graph result");
  require(finished_before_return == 2 && valid_scopes.load(std::memory_order_relaxed) == 2,
          "run returned before untracked native descendants retired");
  require(metadata && !metadata->valid(), "native descendants left their root active");
}
template <class Runtime>
void untracked_blocking_descendant() {
  Runtime runtime;
  std::atomic<unsigned> completed{}, valid_scopes{};
  std::atomic<bool> ordinary_binding{};
  std::shared_ptr<faio::detail::external_root_state> metadata;
  auto result = runtime.run(stdexec::then(stdexec::schedule(runtime.get_scheduler()), [&] {
    require(faio::detail::current_tracker == nullptr, "schedule callback unexpectedly has a native tracker");
    metadata = faio::detail::current_external_scope.root_state->shared_from_this();
    (void)faio::spawn_blocking([&] {
      bool runtime_query_rejected{};
      try { (void)faio::experimental::this_runtime(); }
      catch (const std::logic_error&) { runtime_query_rejected = true; }
      ordinary_binding.store(!faio::detail::on_runtime_worker()
          && faio::detail::current_worker_id() == faio::detail::no_worker_id
          && faio::detail::current_external_host() == &runtime.ref().context().external_host()
          && runtime_query_rejected,
          std::memory_order_release);
      faio::spawn_detached(untracked_descendant(&completed, &valid_scopes));
      return 42;
    });
    return 42;
  }));
  const auto finished_before_return = completed.load(std::memory_order_acquire);
  runtime.shutdown(faio::io::shutdown_policy::drain);
  require(result && std::get<0>(*result) == 42, "blocking descendant changed graph result");
  require(ordinary_binding.load(std::memory_order_acquire), "blocking job lost its ordinary submission binding");
  require(finished_before_return == 1 && valid_scopes.load(std::memory_order_relaxed) == 1,
          "run returned before a blocking job's untracked descendant retired");
  require(metadata && !metadata->valid(), "blocking descendant left its root active");
}

unsigned foreign_context_flags() {
  return (!faio::detail::current_external_scope.valid() ? 1U : 0U)
      | (faio::detail::current_cancellation_policy
              == faio::detail::cancellation_error_policy::fatal_if_unobserved ? 2U : 0U)
      | (!faio::detail::current_cancellation_owner ? 4U : 0U);
}
faio::task<unsigned> foreign_context_probe() { co_return foreign_context_flags(); }
faio::task<void> foreign_context_submit(std::atomic<unsigned>* clean) {
  clean->fetch_add(foreign_context_flags() == 7 ? 1U : 100U, std::memory_order_release);
  co_return;
}
faio::task<unsigned> await_foreign_join(faio::join_handle<unsigned> handle) {
  co_return co_await std::move(handle);
}
template <class Runtime>
faio::task<unsigned> submit_to_foreign_context(Runtime* target, std::atomic<unsigned>* clean) {
  co_return co_await faio::spawn_blocking([target, clean] {
    auto& context = target->ref().context();
    unsigned accepted{};
    accepted += context.block_on(foreign_context_probe()) == 7;
    auto observed = context.spawn_observed(foreign_context_probe());
    accepted += context.block_on(await_foreign_join(std::move(observed))) == 7;
    auto values = context.wait_all(foreign_context_probe(), foreign_context_probe());
    accepted += std::get<0>(values) == 7 && std::get<1>(values) == 7;
    context.submit(foreign_context_submit(clean));
    context.shutdown(faio::io::shutdown_policy::drain);
    return accepted;
  });
}
template <class Runtime>
void foreign_context_isolation() {
  Runtime source, target;
  std::atomic<unsigned> clean{};
  auto result = source.run(source.as_sender(submit_to_foreign_context(&target, &clean)));
  source.shutdown();
  require(result && std::get<0>(*result) == 3 && clean.load(std::memory_order_acquire) == 1,
          "explicit foreign context inherited caller cancellation or external root");
}
template <class Runtime>
void suite() {
  const auto run = [](const char* name, void (*test)()) {
    const auto* mode = std::same_as<Runtime, faio::experimental::current_thread_runtime> ? "current" : "multi";
    const bool trace = std::getenv("FAIO_EXPERIMENTAL_SHUTDOWN_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "%s %s begin\n", mode, name);
    test();
    if (trace) std::fprintf(stderr, "%s %s end\n", mode, name);
  };
  run("reservation", &reservation_shutdown<Runtime>);
  run("metadata", &metadata_shutdown<Runtime>);
  run("worker", &worker_shutdown<Runtime>);
  run("quiescing", &quiescing_continuation<Runtime>);
  run("native-descendants", &untracked_native_descendants<Runtime>);
  run("blocking-descendant", &untracked_blocking_descendant<Runtime>);
  run("foreign-context", &foreign_context_isolation<Runtime>);
}
} // namespace
int main() {
  suite<faio::experimental::current_thread_runtime>();
  suite<faio::experimental::multi_thread_runtime>();
}
