#include <faio/experimental/reflection.h>
#include <faio/faio.hpp>
#include <cassert>

[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> async_main(int argc, char**) {
  co_return argc + 36;
}
int main() {
  constexpr auto options = faio::experimental::runtime_options_of<^^async_main>();
  faio::runtime::configure(faio::experimental::to_runtime_config(options));
  assert(faio::block_on(async_main(1, nullptr)) == 37);
  faio::runtime::shutdown();
}
