#include <faio/experimental/async_main.h>
[[=faio::experimental::main(selected_entry)]]
[[=faio::experimental::runtime_options{}]]
faio::task<int> different_entry(int, char**) { co_return 0; }
