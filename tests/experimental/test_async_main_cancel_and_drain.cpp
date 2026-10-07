#include <faio/faio.hpp>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <faio/experimental/async_main.h>

namespace {
std::atomic<int> started{}, cleaned{};
faio::task<void> background(bool derive = false) {
  struct cleanup { ~cleanup() { cleaned.fetch_add(1); } } guard;
  if (derive) faio::spawn_detached(background());
  started.fetch_add(1);
  for (;;) co_await faio::time::sleep(std::chrono::hours{1});
}
void verify_drain() { assert(cleaned.load() == 4); }
}

[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> async_main(int argc, char** argv) {
  assert(argc > 0 && argv && argv[0]);
  assert(std::atexit(verify_drain) == 0);
  faio::spawn_detached(background(true));
  { auto abandoned = faio::spawn(background()); }
  const auto token = co_await faio::this_coro::stop_token();
  {
    auto abandoned = faio::spawn_blocking([token] {
      struct cleanup { ~cleanup() { cleaned.fetch_add(1); } } guard;
      started.fetch_add(1);
      while (!token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds{1});
      throw faio::operation_cancelled{};
    });
  }
  while (started.load() != 4) co_await faio::this_coro::yield();
  co_return 0;
}
