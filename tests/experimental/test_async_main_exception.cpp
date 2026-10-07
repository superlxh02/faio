#include <faio/faio.hpp>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <stdexcept>
#include <chrono>
#include <faio/experimental/async_main.h>

namespace {
std::atomic<bool> started{}, cleaned{};
faio::task<void> background() {
  struct cleanup { ~cleanup() { cleaned.store(true); } } guard;
  started.store(true);
  for (;;) co_await faio::time::sleep(std::chrono::hours{1});
}
void verify_drain() { assert(cleaned.load()); }
}
[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::multi_thread,
    .workers = 2,
    .io_backend = faio::runtime::io_backend::IO_EPOLL,
    .exit_policy = faio::experimental::entry_exit_policy::drain}]]
faio::task<int> async_main(int, char**) {
  assert(std::atexit(verify_drain) == 0);
  faio::spawn_detached(background());
  while (!started.load()) co_await faio::this_coro::yield();
  throw std::runtime_error("entry test failure");
  co_return 0;
}
