#include <faio/experimental/async_main.h>
[[=faio::experimental::main(launch_app)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> launch_app(int, char**) { co_return 37; }
