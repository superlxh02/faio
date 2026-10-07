#include <faio/experimental/runtime.h>
#include <faio/faio.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <faio/experimental/async_main.h>

namespace {
std::atomic<int> started{}, cleaned{};
faio::task<void> sleeping_child() {
  struct cleanup { ~cleanup() { cleaned.fetch_add(1); } } guard;
  started.fetch_add(1);
  co_await faio::time::sleep(std::chrono::hours{1});
}
faio::task<void> returning_parent() {
  faio::spawn_detached(sleeping_child());
  co_return;
}
void verify_drain() { assert(cleaned.load() == 6); }

void verify_foreign_context() {
  auto* previous_host = faio::detail::current_external_host();
  auto previous_owner = faio::detail::current_cancellation_owner;
  auto options = faio::experimental::current_thread_defaults();
  options.max_blocking_threads = 1;
  options.filesystem_threads = 1;
  options.resolver_threads = 1;
  options.io_backend = faio::runtime::io_backend::IO_EPOLL;
  faio::experimental::current_thread_runtime foreign{options};
  auto* host = &foreign.ref().context().external_host();
  auto job = foreign.ref().context().submit_blocking([host] {
    assert(faio::detail::current_external_host() == host);
    assert(!faio::detail::on_runtime_worker());
    assert(faio::detail::current_worker_id() == faio::detail::no_worker_id);
    assert(faio::detail::current_tracker == nullptr);
    assert(!faio::detail::current_external_scope);
    assert(!faio::detail::current_cancellation_owner);
    assert(faio::detail::current_cancellation_policy
           == faio::detail::cancellation_error_policy::fatal_if_unobserved);
  });
  job.get();
  foreign.shutdown();
  assert(faio::detail::current_external_host() == previous_host);
  assert(faio::detail::current_cancellation_owner == previous_owner);
}
}

[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::multi_thread,
    .workers = 2,
    .max_blocking_threads = 2,
    .filesystem_threads = 1,
    .resolver_threads = 1,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> async_main(int, char**) {
  assert(std::atexit(verify_drain) == 0);
  auto* context = faio::detail::current_external_host()->context();
  auto first = faio::spawn_blocking([context] {
    assert(!faio::detail::on_runtime_worker());
    assert(faio::detail::current_worker_id() == faio::detail::no_worker_id);
    assert(faio::detail::current_external_host()->context() == context);
    faio::spawn_detached(sleeping_child());
    { auto abandoned = faio::spawn(sleeping_child()); }
    { auto nested = faio::spawn_blocking([] { faio::spawn_detached(sleeping_child()); }); }
  });
  co_await std::move(first);
  auto explicit_context = context->submit_blocking([] {
    verify_foreign_context();
    faio::spawn_detached(sleeping_child());
  });
  co_await std::move(explicit_context);
  auto parent = faio::spawn(returning_parent());
  co_await std::move(parent);
  co_await faio::scope([](faio::scope_context&) -> faio::task<void> {
    faio::spawn_detached(sleeping_child());
    co_return;
  });
  while (started.load() != 6) co_await faio::this_coro::yield();
  // Every spawning body and its join state has returned before this stop.
  co_return 0;
}
