#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <exec/task.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <faio/experimental/async_main.h>

namespace {
std::atomic<bool> native_started{}, external_started{}, native_cleaned{}, external_cleaned{};
faio::task<void> sleeping_native(const void* expected_context) {
  assert(std::addressof(faio::experimental::this_runtime().context()) == expected_context);
  struct cleanup { ~cleanup() { native_cleaned.store(true); } } guard;
  native_started.store(true);
  co_await faio::time::sleep(std::chrono::hours{1});
}
exec::task<void> sleeping_external(faio::experimental::runtime_ref runtime) {
  struct cleanup { ~cleanup() { external_cleaned.store(true); } } guard;
  co_await stdexec::schedule(runtime.get_multi_thread_scheduler());
  const auto expected_context = std::addressof(runtime.context());
  assert(std::addressof(faio::experimental::this_runtime().context()) == expected_context);
  external_started.store(true);
  co_await runtime.as_sender(sleeping_native(expected_context));
}
void verify_drain() { assert(native_cleaned.load() && external_cleaned.load()); }
}

[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::multi_thread,
    .workers = 2,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> async_main(int, char**) {
  assert(std::atexit(verify_drain) == 0);
  auto runtime = faio::experimental::this_runtime();
  { auto abandoned = runtime.spawn_sender(sleeping_external(runtime)); }
  while (!native_started.load() || !external_started.load()) co_await faio::this_coro::yield();
  // Entry stop forwards to the independent sender graph and its task bridge.
  co_return 0;
}
