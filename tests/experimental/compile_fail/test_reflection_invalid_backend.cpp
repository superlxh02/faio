#include <faio/experimental/reflection.h>
struct [[=faio::experimental::runtime_options{
    .io_backend = static_cast<faio::runtime::io_backend>(77)}]] invalid {};
constexpr auto options = faio::experimental::runtime_options_of<^^invalid>();
