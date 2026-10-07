#include <faio/experimental/async_main.h>
[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{}]]
faio::task<void> async_main(int, char**) { co_return; }
