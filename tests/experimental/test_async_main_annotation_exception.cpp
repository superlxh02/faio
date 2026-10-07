#include <stdexcept>
#include <faio/experimental/async_main.h>

[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> async_main(int, char**) {
  throw std::runtime_error("annotated entry failure");
  co_return 0;
}
