#include <faio/faio.hpp>
#include <cassert>
#include <cstdlib>
#include <faio/experimental/async_main.h>

namespace {
faio::task<int> early_task() { co_return 19; }
void verify_shutdown() {
  bool rejected{};
  try { (void)faio::block_on(early_task()); }
  catch (const std::logic_error&) { rejected = true; }
  assert(rejected);
}
const bool prestarted = [] {
  faio::runtime::config config{};
  config._mode = faio::runtime::mode::current_thread;
  config._num_workers = 1;
  config._requested_io_backend = faio::runtime::io_backend::IO_EPOLL;
  faio::runtime::configure(config);
  assert(faio::block_on(early_task()) == 19);
  assert(std::atexit(verify_shutdown) == 0);
  return true;
}();
}
[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> async_main(int, char**) {
  // Configuration must reject the already-started default before invoking us.
  std::abort();
  co_return 0;
}
