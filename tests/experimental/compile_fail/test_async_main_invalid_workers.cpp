#include <faio/experimental/async_main.h>
[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread, .workers = 2}]]
faio::task<int> async_main(int, char**) { co_return 0; }
