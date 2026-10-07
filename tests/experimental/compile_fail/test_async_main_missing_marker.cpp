#include <faio/detail/experimental/async_entry.hpp>
[[=faio::experimental::runtime_options{}]]
faio::task<int> entry(int, char**);
constexpr auto options = faio::experimental::detail::entry_options<^^entry>();
