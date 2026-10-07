#include <faio/experimental/reflection.h>
#include <cassert>

using faio::experimental::runtime_options;
using faio::experimental::runtime_options_of;
static_assert(std::meta::nonstatic_data_members_of(^^faio::experimental::runtime_options,
    std::meta::access_context::unprivileged()).size() == 16);
struct plain_entity {};
struct [[=17]] [[=runtime_options{.workers = 3, .io_interval = 7}]] configured_entity {};
struct [[=runtime_options{.io_backend = std::nullopt}]] no_backend_entity {};
struct [[=runtime_options{.io_backend = std::optional{faio::runtime::io_backend::IO_EPOLL}}]]
    optional_backend_entity {};
[[=runtime_options{.mode = faio::runtime::mode::current_thread, .workers = 1}]]
void reachable_declaration();
void reachable_declaration() {}

static_assert(runtime_options_of<^^plain_entity>().workers == 0);
static_assert(runtime_options_of<^^configured_entity>().workers == 3);
static_assert(runtime_options_of<^^configured_entity>().io_interval == 7);
static_assert(!runtime_options_of<^^no_backend_entity>().io_backend);
static_assert(runtime_options_of<^^optional_backend_entity>().io_backend.has_value());
static_assert(runtime_options_of<^^optional_backend_entity>().io_backend.value()
              == faio::runtime::io_backend::IO_EPOLL);
static_assert(runtime_options_of<^^reachable_declaration>().mode == faio::runtime::mode::current_thread);
int main() {
  constexpr auto config = runtime_options_of<^^configured_entity>();
  assert(config.workers == 3);
}
