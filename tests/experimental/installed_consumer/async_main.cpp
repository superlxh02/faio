#include <faio/faio.hpp>
#include <faio/experimental/async_main.h>
[[=faio::experimental::main(program_entry)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread,
    .workers = 1,
    .io_backend = faio::runtime::io_backend::IO_EPOLL
}]]
faio::task<int> program_entry(int argc, char** argv) {
    co_return argc > 0 && argv != nullptr ? 0 : 1;
}
