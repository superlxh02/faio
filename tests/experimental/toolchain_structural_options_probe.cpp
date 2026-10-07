// Reviewable alternative to plan 5.1's non-structural std::optional field.
// This probe does not change the production schema.
#include <faio/experimental/runtime_options.h>
#include <meta>
#include <cassert>
#include <cstdio>
#include <optional>
#include <type_traits>

namespace probe {
using backend = faio::runtime::io_backend;
struct backend_option {
  bool present{};
  backend stored{backend::IO_EPOLL};
  constexpr backend_option() noexcept = default;
  constexpr backend_option(std::nullopt_t) noexcept {}
  constexpr backend_option(backend value) noexcept : present(true), stored(value) {}
  constexpr backend_option(std::optional<backend> value) noexcept
      : present(value.has_value()), stored(value.value_or(backend::IO_EPOLL)) {}
  constexpr explicit operator bool() const noexcept { return present; }
  constexpr bool has_value() const noexcept { return present; }
  constexpr backend operator*() const noexcept { return stored; }
  constexpr backend value_or(backend fallback) const noexcept { return present ? stored : fallback; }
  constexpr operator std::optional<backend>() const noexcept {
    return present ? std::optional<backend>{stored} : std::nullopt;
  }
  friend constexpr bool operator==(backend_option, backend_option) noexcept = default;
};

struct options {
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
  backend_option io_backend{std::nullopt};
  faio::experimental::entry_exit_policy exit_policy{faio::experimental::entry_exit_policy::cancel_and_drain};

  constexpr faio::experimental::runtime_options original() const noexcept {
    return {.mode = mode, .workers = workers, .io_interval = io_interval,
            .global_queue_interval = global_queue_interval, .idle_spin_count = idle_spin_count,
            .num_events = num_events, .max_io_delay_ns = max_io_delay_ns,
            .max_blocking_threads = max_blocking_threads, .blocking_keep_alive_ms = blocking_keep_alive_ms,
            .blocking_queue_limit = blocking_queue_limit, .filesystem_threads = filesystem_threads,
            .resolver_threads = resolver_threads, .filesystem_queue_limit = filesystem_queue_limit,
            .resolver_queue_limit = resolver_queue_limit,
            .io_backend = static_cast<std::optional<backend>>(io_backend), .exit_policy = exit_policy};
  }
};

struct [[=options{.workers = 2, .io_backend = backend::IO_EPOLL}]] backend_entity {};
struct [[=options{.io_backend = std::nullopt}]] nullopt_entity {};
struct [[=options{}]] default_entity {};
inline constexpr std::optional<backend> source{backend::IO_EPOLL};
struct [[=options{.io_backend = source}]] std_optional_entity {};

template <std::meta::info Entity>
consteval options read() {
  const auto annotations = std::meta::annotations_of_with_type(Entity, ^^options);
  if (annotations.size() != 1) throw "probe: expected exactly one options";
  return std::meta::extract<options>(annotations.front());
}
static_assert(std::is_aggregate_v<options> && std::is_trivially_copyable_v<options>);
static_assert(read<^^backend_entity>().io_backend.has_value());
static_assert(*read<^^backend_entity>().io_backend == backend::IO_EPOLL);
static_assert(!read<^^nullopt_entity>().io_backend);
static_assert(!read<^^default_entity>().io_backend);
static_assert(read<^^std_optional_entity>().io_backend.value_or(backend::IO_URING) == backend::IO_EPOLL);
static_assert(faio::experimental::validate_options(options{.io_interval = 0}.original())
              == faio::experimental::options_error::io_interval);
static_assert(faio::experimental::validate_options(options{.io_backend = static_cast<backend>(77)}.original())
              == faio::experimental::options_error::io_backend);
#if !defined(FAIO_HAS_IO_URING) || !FAIO_HAS_IO_URING
static_assert(faio::experimental::validate_options(options{.io_backend = backend::IO_URING}.original())
              == faio::experimental::options_error::io_uring_unavailable);
#endif
}
int main() {
  constexpr auto reflected = probe::read<^^probe::backend_entity>();
  const auto config = faio::experimental::to_runtime_config(reflected.original());
  assert(config._num_workers == 2);
  assert(config._requested_io_backend == probe::backend::IO_EPOLL);
  assert(!probe::read<^^probe::nullopt_entity>().original().io_backend);
  std::printf("structural options P0: compiler=%s libstdc++=%d full-schema annotation/extract/conversion passed\n",
              __VERSION__, _GLIBCXX_RELEASE);
}
