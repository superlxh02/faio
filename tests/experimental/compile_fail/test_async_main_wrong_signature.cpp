#include <faio/detail/experimental/async_entry.hpp>
[[=faio::experimental::main]]
[[=faio::experimental::runtime_options{}]]
faio::task<void> entry(int, char**);
constexpr auto options = faio::experimental::detail::entry_options<^^entry>();
