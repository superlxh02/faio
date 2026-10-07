#include <faio/experimental/async_main.h>
[[=faio::experimental::main(async_main)]]
faio::task<int> entry(int, char**);
constexpr auto options = faio::experimental::detail::entry_options<^^entry>();
