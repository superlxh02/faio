#ifndef FAIO_DETAIL_EXPERIMENTAL_RUNTIME_OPTIONS_HPP
#define FAIO_DETAIL_EXPERIMENTAL_RUNTIME_OPTIONS_HPP

#include "faio/detail/experimental/platform.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

namespace faio::runtime { using config = detail::runtime_config; }

namespace faio::experimental {
enum class entry_exit_policy { cancel_and_drain, drain };

// GCC reflection annotations require structural values. std::optional has
// private state, so retain optional semantics in a public literal value.
struct io_backend_option {
  bool present{};
  faio::runtime::io_backend stored{faio::runtime::io_backend::IO_EPOLL};
  constexpr io_backend_option() noexcept = default;
  constexpr io_backend_option(std::nullopt_t) noexcept {}
  constexpr io_backend_option(faio::runtime::io_backend value) noexcept : present(true), stored(value) {}
  constexpr io_backend_option(std::optional<faio::runtime::io_backend> value) noexcept
      : present(value.has_value()), stored(value.value_or(faio::runtime::io_backend::IO_EPOLL)) {}
  constexpr explicit operator bool() const noexcept { return present; }
  constexpr bool has_value() const noexcept { return present; }
  constexpr faio::runtime::io_backend operator*() const noexcept { return stored; }
  constexpr faio::runtime::io_backend value() const {
    if (!present) throw std::bad_optional_access{};
    return stored;
  }
  constexpr faio::runtime::io_backend value_or(faio::runtime::io_backend fallback) const noexcept {
    return present ? stored : fallback;
  }
  constexpr operator std::optional<faio::runtime::io_backend>() const noexcept {
    return present ? std::optional{stored} : std::nullopt;
  }
  constexpr void reset() noexcept { present = false; stored = faio::runtime::io_backend::IO_EPOLL; }
  friend constexpr bool operator==(io_backend_option left, io_backend_option right) noexcept {
    return left.present == right.present && (!left.present || left.stored == right.stored);
  }
};

struct runtime_options {
  faio::runtime::mode mode{faio::runtime::mode::multi_thread};
  std::size_t workers{};
  std::uint32_t io_interval{61};
  std::uint32_t global_queue_interval{61};
  std::uint32_t idle_spin_count{faio::runtime::detail::AUTO_IDLE_SPIN_COUNT};
  std::size_t num_events{1024};
  std::uint64_t max_io_delay_ns{100000};
  std::size_t max_blocking_threads{64};
  std::uint64_t blocking_keep_alive_ms{10000};
  std::size_t blocking_queue_limit{4096};
  std::size_t filesystem_threads{4};
  std::size_t resolver_threads{2};
  std::size_t filesystem_queue_limit{4096};
  std::size_t resolver_queue_limit{1024};
  io_backend_option io_backend{std::nullopt};
  entry_exit_policy exit_policy{entry_exit_policy::cancel_and_drain};
};

enum class options_error {
  none, mode, workers, io_interval, global_queue_interval, num_events,
  max_io_delay_ns, max_blocking_threads, blocking_keep_alive_ms, blocking_queue_limit,
  filesystem_threads, resolver_threads, filesystem_queue_limit, resolver_queue_limit,
  io_backend, io_uring_unavailable, exit_policy
};

namespace detail {
template <class Rep>
constexpr bool duration_representable(std::uint64_t value) noexcept {
  return value <= static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
}
constexpr const char* options_error_message(options_error error) noexcept {
  switch (error) {
    case options_error::none: return "faio experimental: valid runtime_options";
    case options_error::mode: return "faio experimental: invalid mode";
    case options_error::workers: return "faio experimental: invalid workers";
    case options_error::io_interval: return "faio experimental: invalid io_interval";
    case options_error::global_queue_interval: return "faio experimental: invalid global_queue_interval";
    case options_error::num_events: return "faio experimental: invalid num_events";
    case options_error::max_io_delay_ns: return "faio experimental: invalid max_io_delay_ns";
    case options_error::max_blocking_threads: return "faio experimental: invalid max_blocking_threads";
    case options_error::blocking_keep_alive_ms: return "faio experimental: invalid blocking_keep_alive_ms";
    case options_error::blocking_queue_limit: return "faio experimental: invalid blocking_queue_limit";
    case options_error::filesystem_threads: return "faio experimental: invalid filesystem_threads";
    case options_error::resolver_threads: return "faio experimental: invalid resolver_threads";
    case options_error::filesystem_queue_limit: return "faio experimental: invalid filesystem_queue_limit";
    case options_error::resolver_queue_limit: return "faio experimental: invalid resolver_queue_limit";
    case options_error::io_backend: return "faio experimental: invalid io_backend";
    case options_error::io_uring_unavailable: return "faio experimental: io_uring not compiled";
    case options_error::exit_policy: return "faio experimental: invalid exit_policy";
  }
  return "faio experimental: invalid runtime_options";
}
} // namespace detail

constexpr options_error validate_options(const runtime_options& options) noexcept {
  if (options.mode != faio::runtime::mode::multi_thread
      && options.mode != faio::runtime::mode::current_thread) return options_error::mode;
  if (options.exit_policy != entry_exit_policy::cancel_and_drain
      && options.exit_policy != entry_exit_policy::drain) return options_error::exit_policy;
  if (options.mode == faio::runtime::mode::current_thread && options.workers > 1)
    return options_error::workers;
  if (!options.io_interval) return options_error::io_interval;
  if (!options.global_queue_interval) return options_error::global_queue_interval;
  if (!options.num_events) return options_error::num_events;
  if (!options.max_io_delay_ns
      || !detail::duration_representable<std::chrono::nanoseconds::rep>(options.max_io_delay_ns))
    return options_error::max_io_delay_ns;
  if (!options.max_blocking_threads) return options_error::max_blocking_threads;
  if (!options.blocking_keep_alive_ms
      || !detail::duration_representable<std::chrono::milliseconds::rep>(options.blocking_keep_alive_ms))
    return options_error::blocking_keep_alive_ms;
  if (!options.blocking_queue_limit) return options_error::blocking_queue_limit;
  if (!options.filesystem_threads) return options_error::filesystem_threads;
  if (!options.resolver_threads) return options_error::resolver_threads;
  if (!options.filesystem_queue_limit) return options_error::filesystem_queue_limit;
  if (!options.resolver_queue_limit) return options_error::resolver_queue_limit;
  if (options.io_backend) {
    if (*options.io_backend != faio::runtime::io_backend::IO_EPOLL
        && *options.io_backend != faio::runtime::io_backend::IO_URING) return options_error::io_backend;
#if !defined(FAIO_HAS_IO_URING) || !FAIO_HAS_IO_URING
    if (*options.io_backend == faio::runtime::io_backend::IO_URING) return options_error::io_uring_unavailable;
#endif
  }
  return options_error::none;
}

inline faio::runtime::config to_runtime_config(runtime_options options) {
  if (const auto error = validate_options(options); error != options_error::none)
    throw std::invalid_argument(detail::options_error_message(error));
  faio::runtime::config config{};
  config._mode = options.mode;
  config._num_workers = options.mode == faio::runtime::mode::current_thread
      ? 1 : options.workers ? options.workers : std::max(1u, std::thread::hardware_concurrency());
  config._io_interval = options.io_interval;
  config._global_queue_interval = options.global_queue_interval;
  config._idle_spin_count = options.idle_spin_count;
  config._num_events = options.num_events;
  config._max_io_delay = std::chrono::nanoseconds{
      static_cast<std::chrono::nanoseconds::rep>(options.max_io_delay_ns)};
  config._max_blocking_threads = options.max_blocking_threads;
  config._blocking_keep_alive = std::chrono::milliseconds{
      static_cast<std::chrono::milliseconds::rep>(options.blocking_keep_alive_ms)};
  config._blocking_queue_limit = options.blocking_queue_limit;
  config._filesystem_threads = options.filesystem_threads;
  config._resolver_threads = options.resolver_threads;
  config._filesystem_queue_limit = options.filesystem_queue_limit;
  config._resolver_queue_limit = options.resolver_queue_limit;
  config._requested_io_backend = static_cast<std::optional<faio::runtime::io_backend>>(options.io_backend);
  return faio::runtime::detail::validate_config(config);
}

constexpr runtime_options current_thread_defaults() noexcept {
  return runtime_options{.mode = faio::runtime::mode::current_thread};
}
constexpr runtime_options multi_thread_defaults() noexcept { return {}; }
} // namespace faio::experimental
#endif
