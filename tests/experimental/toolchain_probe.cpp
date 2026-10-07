// P0 spike: the options and runner below are deliberately local to this test.
// This verifies the proposed syntax against the real faio coroutine type before
// any experimental API or runtime changes are introduced.
#include <faio/detail/coroutine/task.hpp>
#include <faio/detail/runtime/common/config.hpp>

#include <cassert>
#include <cstdio>
#include <meta>
#include <type_traits>

namespace faio::experimental {
struct runtime_options {
  faio::runtime::mode mode{faio::runtime::mode::multi_thread};
  std::size_t workers{};
  std::uint32_t io_interval{61};
};

namespace detail {
struct async_entry_marker_type {};
inline constexpr async_entry_marker_type async_entry_marker{};

template <std::meta::info Entity>
consteval runtime_options read_options() {
  const auto annotations = std::meta::annotations_of_with_type(Entity, ^^runtime_options);
  if (annotations.size() != 1)
    throw "faio experimental P0: expected exactly one runtime_options annotation";
  return std::meta::extract<runtime_options>(annotations.front());
}

template <std::meta::info Entity>
int run_main(int faio_argc, char** faio_argv) {
  constexpr auto options = read_options<Entity>();
  static_assert(options.mode == faio::runtime::mode::multi_thread);
  static_assert(options.workers == 4);
  static_assert(options.io_interval == 61);
  static_assert(std::meta::annotations_of_with_type(Entity, ^^async_entry_marker_type).size() == 1);
  static_assert(std::is_same_v<decltype(&[:Entity:]), faio::task<int> (*)(int, char**)>);

  // The probe coroutine has no asynchronous suspension. Production must use the
  // existing runtime, rather than this direct, owned-frame test driver.
  auto child = [:Entity:](faio_argc, faio_argv);
  const auto handle = child.take();
  struct frame_guard {
    decltype(handle) frame;
    ~frame_guard() { frame.destroy(); }
  } guard{handle};
  handle.resume();
  if (!handle.done()) {
    std::fprintf(stderr, "faio experimental P0: probe coroutine unexpectedly suspended\n");
    return 1;
  }
  std::printf("reflection P0: compiler=%s libstdc++=%d headers=%ld cplusplus=%ld\n",
              __VERSION__, _GLIBCXX_RELEASE, static_cast<long>(__GLIBCXX__),
              static_cast<long>(__cplusplus));
  return handle.promise().take_result();
}
}  // namespace detail
}  // namespace faio::experimental

// This must remain the exact macro shape required by section 6 of the plan.
#define ASYNC(...)                                                              \
  experimental::detail::async_entry_marker]]                                   \
  [[=__VA_ARGS__]] faio::task<int> async_main(int, char**);                       \
  int main(int faio_argc, char** faio_argv) {                                    \
    return faio::experimental::detail::run_main<^^async_main>(faio_argc,         \
                                                             faio_argv);       \
  }                                                                            \
  [[maybe_unused

[[=faio::ASYNC(faio::experimental::runtime_options{
    .mode = faio::runtime::mode::multi_thread,
    .workers = 4,
    .io_interval = 61
})]]
faio::task<int> async_main(int argc, char** argv) {
  co_return argc > 0 && argv != nullptr && argv[0] != nullptr ? 0 : 1;
}
