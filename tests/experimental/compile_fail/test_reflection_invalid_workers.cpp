#include <faio/experimental/reflection.h>
struct [[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread, .workers = 2}]] invalid {};
constexpr auto options = faio::experimental::runtime_options_of<^^invalid>();
