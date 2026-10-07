#include <faio/experimental/reflection.h>
struct [[=faio::experimental::runtime_options{.blocking_keep_alive_ms = UINT64_MAX}]] invalid {};
constexpr auto options = faio::experimental::runtime_options_of<^^invalid>();
