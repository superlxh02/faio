#include <faio/faio.hpp>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <faio/experimental/async_main.h>

namespace {
std::atomic<bool> finished{};
faio::task<void> background() {
  for (int i = 0; i != 100; ++i) co_await faio::this_coro::yield();
  assert(!(co_await faio::this_coro::stop_token()).stop_requested());
  finished.store(true);
}
void verify_drain() { assert(finished.load()); }
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
  co_return 0;
}
