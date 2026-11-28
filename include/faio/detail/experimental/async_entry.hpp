/**
 * @file faio/detail/experimental/async_entry.hpp
 * @brief 反射异步入口。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_ASYNC_ENTRY_HPP
#define FAIO_DETAIL_EXPERIMENTAL_ASYNC_ENTRY_HPP

#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/experimental/reflection.hpp"
#include "faio/detail/coroutine/launch/root_task.hpp"
#include "faio/detail/runtime/default.hpp"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stop_token>
#include <type_traits>

namespace faio::experimental {
/** @brief 标识并关联反射异步入口函数的注解值。 */
struct main_annotation_type {
  /** @brief 构造标记给定异步入口的注解。 */
  consteval main_annotation_type operator()(faio::task<int> (&)(int, char**)) const { return {}; }
};

inline constexpr main_annotation_type main{};

namespace detail {

/** @brief 反射给定异步入口函数。 */
consteval std::meta::info entry_function_of(faio::task<int> (&entry)(int, char**)) {
  return std::meta::reflect_function(entry);
}

/** @brief 异步入口及其派生任务的停止、结果与排空状态。 */
struct entry_scope {
  std::stop_source stop{}; /**< 入口停止源，向已登记的等待者广播协作取消请求。 */
  faio::detail::task_tracker
      tracker{}; /**< 跟踪本任务及其派生任务的完成数量，计数归零后才允许排空操作返回。 */
  std::optional<int> result{}; /**< 异步入口完成后保存的返回值。 */
  std::exception_ptr error{};  /**< 跨异步边界保存异常，在领取结果时重新抛出。 */
  entry_exit_policy exit_policy{
      entry_exit_policy::CANCEL_AND_DRAIN}; /**< 入口任务返回后对派生任务采用的取消或排空策略。 */
  faio::detail::cancellation_error_policy cancellation_policy{
      faio::detail::cancellation_error_policy::
          NORMAL_IF_STOP_REQUESTED}; /**< 随任务上下文传递的策略，用于处理未观察的取消异常。 */
};

/** @brief 将运行时停止请求转发给入口停止源。 */
struct entry_forward_stop {
  std::stop_source* source; /**< 入口停止源，向已登记的等待者广播协作取消请求。 */

  /** @brief 向入口停止源转发停止请求。 */
  void operator()() const noexcept { source->request_stop(); }
};

/** @brief 验证异步入口，读取显式选项或使用默认选项。 */
template <std::meta::info Entity>
consteval runtime_options entry_options() {
  if (std::meta::annotations_of_with_type(Entity, ^^main_annotation_type).size() != 1)
    throw "faio experimental: entry requires exactly one faio::experimental::main annotation";
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

/** @brief 执行异步入口并保存结果，按退出策略向派生任务请求协作停止。 */
template <std::meta::info Entity>
faio::detail::root_task entry_root_task_wrapper(entry_scope* scope, int argc, char** argv) {
  try {
    scope->result.emplace(co_await [:Entity:](argc, argv));
  } catch (...) {
    scope->error = std::current_exception();
  }
  if (scope->error || scope->exit_policy == entry_exit_policy::CANCEL_AND_DRAIN)
    scope->stop.request_stop();
}

/** @brief 构造入口根任务，并安装任务组、停止令牌与取消策略上下文。 */
template <std::meta::info Entity>
faio::detail::root_task make_entry_root_task(entry_scope* scope, int argc, char** argv) {
  auto root = entry_root_task_wrapper<Entity>(scope, argc, argv);
  root.set_context({.scope = &scope->tracker,
                    .stop_token = scope->stop.get_token(),
                    .cancellation_policy = scope->cancellation_policy});
  return root;
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
      auto root = make_entry_root_task<Entity>(&scope, faio_argc, faio_argv);
      scope.tracker.add();
      context.run_entry(root, scope.tracker);
      if (scope.error)
        std::rethrow_exception(scope.error);
      return scope.result.value();
    });
  } catch (...) {
    error = std::current_exception();
  }
  if (service) {
    try {
      service->shutdown(faio::io::shutdown_policy::CANCEL_ALL);
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
}  // namespace detail
}  // namespace faio::experimental
#endif
