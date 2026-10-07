#include <faio/experimental/async_main.h>
[[=faio::experimental::main]]
[[=faio::experimental::runtime_options{}]]
faio::task<int> custom_entry(int, char**) { co_return 0; }
