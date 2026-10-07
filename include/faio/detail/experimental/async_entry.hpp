#ifndef FAIO_DETAIL_EXPERIMENTAL_ASYNC_ENTRY_HPP
#define FAIO_DETAIL_EXPERIMENTAL_ASYNC_ENTRY_HPP

#include "faio/detail/experimental/reflection.hpp"
#include "faio/detail/coroutine/root_task.hpp"
#include "faio/detail/runtime/default.hpp"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stop_token>
#include <type_traits>

namespace faio::experimental {
struct main_annotation_type {
  consteval main_annotation_type operator()(faio::task<int> (&)(int, char**)) const {
    return {};
  }
};
inline constexpr main_annotation_type main{};

namespace detail {

consteval std::meta::info entry_function_of(faio::task<int> (&entry)(int, char**)) {
  return std::meta::reflect_function(entry);
}

struct entry_scope {
  std::stop_source stop{};
  faio::detail::task_tracker tracker{};
  std::optional<int> result{};
  std::exception_ptr error{};
  entry_exit_policy exit_policy{entry_exit_policy::cancel_and_drain};
  faio::detail::cancellation_error_policy cancellation_policy{
      faio::detail::cancellation_error_policy::normal_if_stop_requested};
};

struct entry_forward_stop {
  std::stop_source* source;
  void operator()() const noexcept { source->request_stop(); }
};

template <std::meta::info Entity>
consteval runtime_options entry_options() {
  if (std::meta::annotations_of_with_type(Entity, ^^main_annotation_type).size() != 1)
    throw "faio experimental: entry requires exactly one faio::experimental::main annotation";
  const auto options = std::meta::annotations_of_with_type(Entity, ^^runtime_options);
  if (options.empty())
    throw "faio experimental: entry requires runtime_options";
  static_assert(std::meta::is_function(Entity),
                "faio experimental: entry signature must be faio::task<int>(int, char**)");
  if constexpr (std::meta::is_function(Entity)) {
    if (std::meta::parent_of(Entity) != ^^::)
      throw "faio experimental: entry must be in global namespace";
    static_assert(std::is_same_v<decltype(&[:Entity:]), faio::task<int> (*)(int, char**)>,
                  "faio experimental: entry signature must be faio::task<int>(int, char**)");
  }
  return runtime_options_of<Entity>();
}

template <std::meta::info Entity>
faio::detail::detached_task make_entry_coro(entry_scope* scope, int argc, char** argv) {
  faio::detail::current_tracker = &scope->tracker;
  faio::detail::current_stop_token = scope->stop.get_token();
  faio::detail::current_cancellation_policy = scope->cancellation_policy;
  faio::detail::current_external_scope = {};
  faio::detail::current_cancellation_owner.reset();
  try {
    scope->result.emplace(co_await [:Entity:](argc, argv));
  } catch (...) {
    scope->error = std::current_exception();
  }
  // Stop before returning this root's ticket; all derived roots share tracker.
  if (scope->error || scope->exit_policy == entry_exit_policy::cancel_and_drain)
    scope->stop.request_stop();
}

template <std::meta::info Entity>
int run_main(int faio_argc, char** faio_argv) noexcept {
  constexpr auto options = entry_options<Entity>();
  faio::runtime::detail::default_runtime_service* service{};
  std::exception_ptr error;
  int result = EXIT_FAILURE;
  try {
    const auto config = to_runtime_config(options);
    service = &faio::runtime::detail::default_service();
    service->configure(config);
    result = service->with_context([&](faio::runtime::detail::runtime_context& context) {
      entry_scope scope{.exit_policy = options.exit_policy};
      std::stop_callback parent_stop{context.stop_token(), entry_forward_stop{&scope.stop}};
      // Both user invocation and descendants execute within this single scope.
      auto root = make_entry_coro<Entity>(&scope, faio_argc, faio_argv);
      scope.tracker.add();
      context.run_entry(root, scope.tracker);
      if (scope.error)
        std::rethrow_exception(scope.error);
      return scope.result.value();
    });
  } catch (...) {
    error = std::current_exception();
  }
  // The controlled access and shared lock have ended before shutdown.
  if (service) {
    try {
      service->shutdown(faio::io::shutdown_policy::cancel_all);
    } catch (...) {
      if (!error)
        error = std::current_exception();
    }
  }
  if (error) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& failure) {
      std::fprintf(stderr, "faio experimental: entry failed: %s\n", failure.what());
    } catch (...) {
      std::fputs("faio experimental: entry failed with unknown exception\n", stderr);
    }
    return EXIT_FAILURE;
  }
  return result;
}
} // namespace detail
} // namespace faio::experimental
#endif
