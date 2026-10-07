#include <faio/experimental/runtime_options.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>

using namespace faio::experimental;
static_assert(std::is_aggregate_v<runtime_options>);
static_assert(std::is_trivially_copyable_v<runtime_options>);
static_assert(validate_options(runtime_options{}) == options_error::none);
static_assert(validate_options(runtime_options{.idle_spin_count = 0}) == options_error::none);
static_assert(validate_options(runtime_options{.mode = faio::runtime::mode::current_thread,
                                               .workers = 2}) == options_error::workers);
static_assert(validate_options(runtime_options{.io_interval = 0}) == options_error::io_interval);
static_assert(validate_options(runtime_options{.max_io_delay_ns = UINT64_MAX})
              == options_error::max_io_delay_ns);
static_assert(validate_options(runtime_options{.blocking_keep_alive_ms = UINT64_MAX})
              == options_error::blocking_keep_alive_ms);
static_assert(validate_options(runtime_options{.mode = static_cast<faio::runtime::mode>(99)})
              == options_error::mode);
static_assert(validate_options(runtime_options{.exit_policy = static_cast<entry_exit_policy>(99)})
              == options_error::exit_policy);
static_assert(validate_options(runtime_options{.global_queue_interval = 0}) == options_error::global_queue_interval);
static_assert(validate_options(runtime_options{.num_events = 0}) == options_error::num_events);
static_assert(validate_options(runtime_options{.max_io_delay_ns = 0}) == options_error::max_io_delay_ns);
static_assert(validate_options(runtime_options{.max_blocking_threads = 0}) == options_error::max_blocking_threads);
static_assert(validate_options(runtime_options{.blocking_keep_alive_ms = 0}) == options_error::blocking_keep_alive_ms);
static_assert(validate_options(runtime_options{.blocking_queue_limit = 0}) == options_error::blocking_queue_limit);
static_assert(validate_options(runtime_options{.filesystem_threads = 0}) == options_error::filesystem_threads);
static_assert(validate_options(runtime_options{.resolver_threads = 0}) == options_error::resolver_threads);
static_assert(validate_options(runtime_options{.filesystem_queue_limit = 0}) == options_error::filesystem_queue_limit);
static_assert(validate_options(runtime_options{.resolver_queue_limit = 0}) == options_error::resolver_queue_limit);
static_assert(validate_options(runtime_options{.io_backend = static_cast<faio::runtime::io_backend>(99)})
              == options_error::io_backend);

int main() {
  const runtime_options options{
      .mode = faio::runtime::mode::current_thread,
      .workers = 0,
      .io_interval = 13,
      .global_queue_interval = 17,
      .idle_spin_count = 0,
      .num_events = 19,
      .max_io_delay_ns = 23,
      .max_blocking_threads = 2,
      .blocking_keep_alive_ms = 29,
      .blocking_queue_limit = 31,
      .filesystem_threads = 3,
      .resolver_threads = 4,
      .filesystem_queue_limit = 37,
      .resolver_queue_limit = 41,
      .io_backend = faio::runtime::io_backend::IO_EPOLL,
      .exit_policy = entry_exit_policy::drain};
  const auto config = to_runtime_config(options);
  assert(config._mode == options.mode && config._num_workers == 1);
  assert(config._io_interval == 13 && config._global_queue_interval == 17);
  assert(config._idle_spin_count == 0 && config._num_events == 19);
  assert(config._max_io_delay == std::chrono::nanoseconds{23});
  assert(config._max_blocking_threads == 2);
  assert(config._blocking_keep_alive == std::chrono::milliseconds{29});
  assert(config._blocking_queue_limit == 31 && config._filesystem_threads == 3);
  assert(config._resolver_threads == 4 && config._filesystem_queue_limit == 37);
  assert(config._resolver_queue_limit == 41);
  assert(config._requested_io_backend == faio::runtime::io_backend::IO_EPOLL);
  io_backend_option optional{std::optional{faio::runtime::io_backend::IO_EPOLL}};
  assert(optional == faio::runtime::io_backend::IO_EPOLL);
  optional.reset();
  assert(optional == std::nullopt);
  assert(optional.value_or(faio::runtime::io_backend::IO_URING) == faio::runtime::io_backend::IO_URING);
  assert(!static_cast<std::optional<faio::runtime::io_backend>>(optional));
  assert(to_runtime_config(multi_thread_defaults())._num_workers >= 1);
  bool rejected{};
  try { (void)to_runtime_config(runtime_options{.workers = 1, .io_interval = 0}); }
  catch (const std::invalid_argument& error) {
    rejected = std::string_view{error.what()}.find("invalid io_interval") != std::string_view::npos;
  }
  assert(rejected);
#if !defined(FAIO_HAS_IO_URING) || !FAIO_HAS_IO_URING
  static_assert(validate_options(runtime_options{.io_backend = faio::runtime::io_backend::IO_URING})
                == options_error::io_uring_unavailable);
#endif
}
