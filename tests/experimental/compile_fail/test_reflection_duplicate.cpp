#include <faio/experimental/reflection.h>
struct [[=faio::experimental::runtime_options{}]]
       [[=faio::experimental::runtime_options{.workers = 2}]] duplicate {};
constexpr auto options = faio::experimental::runtime_options_of<^^duplicate>();
