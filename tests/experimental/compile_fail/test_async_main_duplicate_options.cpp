#include <faio/experimental/async_main.h>
[[=faio::experimental::runtime_options{}]]
[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{.workers = 1}]]
faio::task<int> async_main(int, char**) { co_return 0; }
