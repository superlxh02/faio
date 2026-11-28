/**
 * @file faio/detail/runtime/context.hpp
 * @brief 上下文与服务入口。
 */
#ifndef FAIO_DETAIL_RUNTIME_CONTEXT_HPP
#define FAIO_DETAIL_RUNTIME_CONTEXT_HPP

#include "faio/detail/coroutine/launch/observed_task.hpp"
#include "faio/detail/coroutine/composition/join_handle.hpp"
#include "faio/detail/coroutine/launch/root_task.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/runtime/common/blocking_task.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/current_thread/runtime.hpp"
#include "faio/detail/runtime/multi_thread/poller.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/context.hpp"
#endif
#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace faio::runtime::detail {
using ::faio::detail::root_task;
using ::faio::detail::schedule_task;
using ::faio::detail::task_tracker;

/** @brief 运行时同步等待的结果存储槽。 */
template <class T>
struct result_slot {
  std::optional<T> value;       /**< 保存同步等待的结果值，读取前须确认操作已完成。 */
  std::exception_ptr exception; /**< 跨异步边界保存的异常，结果领取时重新抛出。 */

  /** @brief 若结果槽保存了异常，则在结果消费线程重新抛出。 */
  void get_error() const {
    if (exception)
      std::rethrow_exception(exception);
  }

  /** @brief 领取结果槽中的值；存在异常时先重抛异常。 */
  T get() {
    get_error();
    return std::move(value.value());
  }
};

/** @brief 运行时同步等待的结果存储槽。 */
template <>
struct result_slot<void> {
  std::exception_ptr exception; /**< 跨异步边界保存的异常，结果领取时重新抛出。 */

  /** @brief 检查无返回值任务的结果；存在异常时重抛异常。 */
  void get() {
    if (exception)
      std::rethrow_exception(exception);
  }
};

/** @brief 执行同步等待所驱动的根协程。 */
template <class T, class S>
root_task block_on_root_task_wrapper(task<T, S> child, result_slot<T>* slot) {
  try {
    if constexpr (std::is_void_v<T>)
      co_await std::move(child);
    else
      slot->value.emplace(co_await std::move(child));
  } catch (...) {
    slot->exception = std::current_exception();
  }
}

/** @brief 构造同步等待的根任务，并配置任务组与停止令牌上下文。 */
template <class T, class S>
root_task make_block_on_root_task(task<T, S> child,
                                  result_slot<T>* slot,
                                  task_tracker* tracker,
                                  std::stop_token stop = {}) {
  auto context = ::faio::detail::capture_task_context();
  context.scope = tracker;
  context.stop_token = std::move(stop);
  auto root = block_on_root_task_wrapper(std::move(child), slot);
  root.set_context(std::move(context));
  return root;
}

/**  @brief 无结果根任务的运行时包装；因协作停机而取消视为正常退出，其他异常仍须处理。 */
template <class T, class S>
root_task runtime_submit_root_task_wrapper(task<T, S> child) {
  try {
    co_await std::move(child);
  } catch (const operation_cancelled&) {
  }
}

/** @brief 构造无结果观察者的运行时根任务，并配置运行时停止令牌。 */
template <class T, class S>
root_task make_runtime_submit_root_task(task<T, S> child, std::stop_token stop) {
  auto context = ::faio::detail::capture_task_context();
  context.scope = nullptr;
  context.stop_token = std::move(stop);
  auto root = runtime_submit_root_task_wrapper(std::move(child));
  root.set_context(std::move(context));
  return root;
}

/** @brief 运行时服务与调度上下文。 */
class runtime_context
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    : public runtime_execution_adapter
#endif
{
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  friend class runtime_execution_adapter;
#endif
 public:
  runtime_context() : runtime_context(runtime_config{}) {}

  explicit runtime_context(runtime_config config) : config_(validate_config(std::move(config))) {
    if (config_._mode == runtime::mode::CURRENT_THREAD)
      current_ = std::make_unique<current_thread_runtime>(config_, this);
    else
      poller_ = std::make_unique<runtime_poller>(config_, this);
  }

  ~runtime_context() {
    try {
      stop(io::shutdown_policy::DRAIN);
    } catch (...) {
      std::terminate();
    }
  }

  runtime_context(const runtime_context&) = delete;

  runtime_context& operator=(const runtime_context&) = delete;

  const runtime_config& config() const noexcept { return config_; }

  bool running() const noexcept {
    return accepting_.load(std::memory_order_acquire) && (!!poller_ || !!current_);
  }

  io::io_capabilities io_capabilities() const noexcept {
    return !stopped_.load(std::memory_order_acquire) && current_ ? current_->capabilities()
           : !stopped_.load(std::memory_order_acquire) && poller_
               ? poller_->shared()->capabilities()
               : io::io_capabilities{
                     false, false, false, false, false, false, false, false, "stopped"};
  }
#if defined(__linux__)
  runtime::io_backend selected_io_backend() const noexcept {
    return io::detail::compiled_io_backend;
  }
#endif
  scheduler_ref scheduler() {
    if (!accepting_.load(std::memory_order_acquire))
      throw std::logic_error("runtime_context 已停止接受任务");
    if (current_)
      return current_->scheduler();
    if (poller_)
      return poller_->shared()->scheduler_reference();
    throw std::logic_error("runtime_context 已停止");
  }

  task_lifetime_ref task_lifetime() {
    if (current_)
      return current_->lifetime();
    if (poller_)
      return poller_->shared()->task_lifetime_reference();
    throw std::logic_error("runtime_context 已停止");
  }

  std::stop_token stop_token() const noexcept { return stop_.get_token(); }

  void request_stop() noexcept {
    stop_.request_stop();
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    if (current_)
      current_->external_host().request_stop();
    else if (poller_)
      poller_->shared()->external_host().request_stop();
#else
    if (current_)
      current_->completion_service().notify_completion();
    else if (poller_)
      poller_->shared()->completion_service().notify_completion();
#endif
  }

  template <class Predicate>
  void drive_until(Predicate done) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 同步驱动运行时");
    if (current_)
      current_->drive_until(std::move(done));
    else if (poller_)
      poller_->shared()->completion_service().wait_until(std::move(done));
    else
      throw std::logic_error("runtime_context 已停止");
  }

  void run_entry(root_task& root, task_tracker& tracker) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 同步运行入口");
    ::faio::detail::scoped_task_context submission{
        {.stop_token = stop_.get_token(), .scheduling = inherited_scheduling()}};
    auto driver_affinity = prepare_driver_affinity();
    schedule_task(std::move(root), scheduler(), &tracker, task_lifetime());
    if (current_)
      current_->drive_until_prebound(tracker);
    tracker.wait();
  }

  void stop(io::shutdown_policy policy = io::shutdown_policy::DRAIN) { stop_impl(policy, false); }

  void shutdown(io::shutdown_policy policy = io::shutdown_policy::CANCEL_ALL) { stop(policy); }

  template <class S = default_scheduler, class F>
    requires std::same_as<S, default_scheduler>
  auto submit_blocking(F&& function) {
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    return submit_blocking_on_graph<S>(std::forward<F>(function));
#else
    const auto target = scheduler();
    const bool inherit = ::faio::detail::current_scheduler() == target;
    auto& pool = current_ ? current_->blocking() : poller_->shared()->blocking();
    return submit_blocking_task<S>(
        pool,
        std::forward<F>(function),
        inherit ? ::faio::detail::current_task_context().scope : nullptr,
        inherit ? ::faio::detail::current_task_context().stop_token : stop_.get_token(),
        task_lifetime(),
        target);
#endif
  }

  template <class T>
  join_handle<T> spawn_observed(task<T> child) {
    ::faio::detail::scoped_task_context submission{
        {.stop_token = stop_.get_token(), .scheduling = inherited_scheduling()}};
    return join_handle<T>{::faio::detail::schedule_observed_task(
        scheduler(), std::move(child), nullptr, stop_.get_token(), task_lifetime())};
  }

  template <class T>
  void submit(task<T> child) {
    if (!running())
      throw std::logic_error("runtime_context 已停止");
    ::faio::detail::scoped_task_context submission{
        {.stop_token = stop_.get_token(), .scheduling = inherited_scheduling()}};
    const auto target = scheduler();
    auto root = make_runtime_submit_root_task(std::move(child), stop_.get_token());
    schedule_task(std::move(root), target, nullptr, task_lifetime());
  }

  template <class T>
  T block_on(task<T> child) {
    if (!running())
      throw std::logic_error("runtime_context 已停止");
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 线程调用 block_on；请 co_await task");
    auto driver_affinity = prepare_driver_affinity();
    ::faio::detail::scoped_task_context submission{
        {.stop_token = stop_.get_token(), .scheduling = inherited_scheduling()}};
    const auto target = scheduler();
    result_slot<T> slot;
    task_tracker tracker;
    tracker.add();
    auto root = make_block_on_root_task(std::move(child), &slot, &tracker, stop_.get_token());
    schedule_task(std::move(root), target, &tracker, task_lifetime());
    if (current_)
      current_->drive_until_prebound(tracker);
    tracker.wait();
    return slot.get();
  }

  template <class... Ts>
  std::tuple<Ts...> wait_all(task<Ts>... tasks) {
    if (!running())
      throw std::logic_error("runtime_context 已停止");
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 线程调用 wait_all；请 co_await join");
    auto driver_affinity = prepare_driver_affinity();
    ::faio::detail::scoped_task_context submission{
        {.stop_token = stop_.get_token(), .scheduling = inherited_scheduling()}};
    static_assert((!std::is_void_v<Ts> && ...), "wait_all 不支持 void；请使用 join");
    if constexpr (sizeof...(Ts) == 0)
      return {};
    else {
      auto slots = std::tuple<result_slot<Ts>...>{};
      task_tracker tracker;
      tracker.add();
      try {
        submit_all(tracker, slots, std::index_sequence_for<Ts...>{}, std::move(tasks)...);
      } catch (...) {
        tracker.done();
        if (current_)
          current_->drive_until_prebound(tracker);
        tracker.wait();
        throw;
      }
      tracker.done();
      if (current_)
        current_->drive_until_prebound(tracker);
      tracker.wait();
      return collect(slots, std::index_sequence_for<Ts...>{});
    }
  }

 private:
  friend class default_runtime_service;

  ::faio::detail::task_scheduling_policy inherited_scheduling() {
    return ::faio::detail::current_scheduler_matches(scheduler())
               ? ::faio::detail::current_task_context().scheduling
               : ::faio::detail::task_scheduling_policy{};
  }

  void stop_prebound(io::shutdown_policy policy) { stop_impl(policy, true); }

  void stop_impl(io::shutdown_policy policy, bool affinity_prepared) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 同步关闭运行时");
    std::lock_guard stop_lock(stop_mutex_);
    if (stopped_.load(std::memory_order_acquire))
      return;
    ::faio::detail::scoped_thread_context cleanup_context;
    auto driver_affinity = prepare_driver_affinity(affinity_prepared);
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    external_host().begin_quiescing();
#endif
    accepting_.store(false, std::memory_order_release);
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    external_host().begin_draining();
#endif
    if (policy == io::shutdown_policy::CANCEL_ALL)
      request_stop();
    if (current_)
      current_->stop_prebound(policy);
    if (poller_)
      poller_->shutdown(policy);
    stopped_.store(true, std::memory_order_release);
  }

  std::optional<worker_affinity_scope> prepare_driver_affinity(
      bool already_prepared = false) const {
    if (!already_prepared && current_ && !config_._worker_cpus.empty()
        && !stopped_.load(std::memory_order_acquire))
      return std::optional<worker_affinity_scope>{std::in_place, config_._worker_cpus};
    return {};
  }

  template <class S, class... Ts, std::size_t... Is>
  void submit_all(task_tracker& tracker,
                  std::tuple<result_slot<Ts>...>& slots,
                  std::index_sequence<Is...>,
                  task<Ts, S>... tasks) {
    (submit_one(tracker, std::get<Is>(slots), std::move(tasks)), ...);
  }

  template <class T>
  void submit_one(task_tracker& tracker, result_slot<T>& slot, task<T> child) {
    const auto target = scheduler();
    auto root = make_block_on_root_task(std::move(child), &slot, &tracker, stop_.get_token());
    tracker.add();
    schedule_task(std::move(root), target, &tracker, task_lifetime());
  }

  template <class... Ts, std::size_t... Is>
  static std::tuple<Ts...> collect(std::tuple<result_slot<Ts>...>& slots,
                                   std::index_sequence<Is...>) {
    return {std::get<Is>(slots).get()...};
  }

  std::stop_source stop_;
  std::mutex stop_mutex_;
  std::atomic<bool> accepting_{true};
  std::atomic<bool> stopped_{false};
  runtime_config config_;
  std::unique_ptr<runtime_poller> poller_;
  std::unique_ptr<current_thread_runtime> current_;
};
}  // namespace faio::runtime::detail
#endif
