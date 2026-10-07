#include <faio/experimental/reflection.h>
struct [[=faio::experimental::runtime_options{.io_interval = 0}]] invalid {};
constexpr auto options = faio::experimental::runtime_options_of<^^invalid>();
