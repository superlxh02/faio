#include <faio/experimental/reflection.h>
struct [[=faio::experimental::runtime_options{.max_io_delay_ns = UINT64_MAX}]] invalid {};
constexpr auto options = faio::experimental::runtime_options_of<^^invalid>();
