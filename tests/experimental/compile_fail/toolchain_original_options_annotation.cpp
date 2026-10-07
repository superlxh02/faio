// Preserved plan 5.1 schema: std::optional prevents structural annotations.
#include <faio/detail/runtime/common/config.hpp>
#include <meta>
namespace original_plan {
enum class entry_exit_policy { cancel_and_drain, drain };
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
  std::optional<faio::runtime::io_backend> io_backend{std::nullopt};
  entry_exit_policy exit_policy{entry_exit_policy::cancel_and_drain};
};
struct [[=runtime_options{}]] annotated {};
}
