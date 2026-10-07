#ifndef FAIO_DETAIL_RUNTIME_CONTEXT_HPP
#define FAIO_DETAIL_RUNTIME_CONTEXT_HPP

#include "faio/detail/coroutine/root_task.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/runtime/common/blocking_task.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/current_thread/runtime.hpp"
#include "faio/detail/runtime/multi_thread/poller.hpp"
#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace faio::runtime::detail {
using ::faio::detail::current_tracker;
using ::faio::detail::detached_task;
using ::faio::detail::spawn_coro;
using ::faio::detail::start_detached;
using ::faio::detail::task_tracker;

template <class T>
struct result_slot {
  std::optional<T> value;
  std::exception_ptr exception;

  void get_error() const {
    if (exception)
      std::rethrow_exception(exception);
  }

  T get() {
    get_error();
    return std::move(value.value());
  }
};

template <>
struct result_slot<void> {
  std::exception_ptr exception;

  void get() {
    if (exception)
      std::rethrow_exception(exception);
  }
};

template <class T>
detached_task block_coro(task<T> child,
                         result_slot<T>* slot,
                         task_tracker* tracker,
                         std::stop_token stop = {}) {
  current_tracker = tracker;
  ::faio::detail::current_stop_token = stop;
  try {
    if constexpr (std::is_void_v<T>)
      co_await std::move(child);
    else
      slot->value.emplace(co_await std::move(child));
  } catch (...) {
    slot->exception = std::current_exception();
  }
  current_tracker = nullptr;
}

/** @brief 无结果 root 的运行时包装；协作停机取消是正常退出，其他异常仍须处理。
 */
template <class T>
detached_task runtime_spawn_coro(task<T> child, std::stop_token stop) {
  ::faio::detail::current_tracker = nullptr;
  ::faio::detail::current_stop_token = stop;
  try {
    co_await std::move(child);
  } catch (const operation_cancelled&) {
  }  // cancel_all 的 sleep/yield 不应触发无人观察异常终止。
}

class runtime_context {
 public:
  runtime_context() : runtime_context(runtime_config{}) {}

  explicit runtime_context(runtime_config config) : config_(validate_config(config)) {
    if (config_._mode == runtime::mode::current_thread)
      current_ = std::make_unique<current_thread_runtime>(config_, this);
    else
      poller_ = std::make_unique<runtime_poller>(config_, this);
  }

  ~runtime_context() {
    try { stop(io::shutdown_policy::drain); }
    catch (...) { std::terminate(); }
  }

  runtime_context(const runtime_context&) = delete;

  runtime_context& operator=(const runtime_context&) = delete;

  const runtime_config& config() const noexcept { return config_; }

  bool running() const noexcept {
    return accepting_.load(std::memory_order_acquire) && (!!poller_ || !!current_);
  }

  /** @brief 查询已初始化驱动器能力，不在 read/write 热路径探测平台。 */
  io::io_capabilities io_capabilities() const noexcept {
    return !stopped_.load(std::memory_order_acquire) && current_ ? current_->capabilities()
           : !stopped_.load(std::memory_order_acquire) && poller_ ? poller_->shared()->capabilities()
                     : io::io_capabilities{
                           false, false, false, false, false, false, false, false, "stopped"};
  }
#if defined(__linux__)
  runtime::io_backend selected_io_backend() const noexcept { return runtime::io_backend::IO_EPOLL; }
#endif
  // 查询纯调度入口；停止后拒绝创建指向已销毁调度域的借用引用。
  scheduler_ref scheduler() {
    if (!accepting_.load(std::memory_order_acquire))
      throw std::logic_error("runtime_context 已停止接受任务");
    if (current_)
      return current_->scheduler();
    if (poller_)
      return poller_->shared()->scheduler_reference();
    throw std::logic_error("runtime_context 已停止");
  }

  // 查询根任务计数服务，供外部提交入口与调度引用一起传给根帧。
  task_lifetime_ref task_lifetime() {
    if (current_)
      return current_->lifetime();
    if (poller_)
      return poller_->shared()->task_lifetime_reference();
    throw std::logic_error("runtime_context 已停止");
  }

  ::faio::detail::external_work_host& external_host() {
    if (current_)
      return current_->external_host();
    if (poller_)
      return poller_->shared()->external_host();
    throw std::logic_error("runtime_context 已停止");
  }
  scheduler_ref scheduler_for_external_root(const ::faio::detail::external_work_host::root_handle& root) {
    if (!external_host().owns(root))
      throw std::logic_error("external root does not belong to this runtime");
    return current_ ? current_->scheduler() : poller_->shared()->scheduler_reference();
  }
  task_lifetime_ref task_lifetime_for_external_root(const ::faio::detail::external_work_host::root_handle& root) {
    if (!external_host().owns(root))
      throw std::logic_error("external root does not belong to this runtime");
    return task_lifetime();
  }
  std::stop_token stop_token() const noexcept { return stop_.get_token(); }
  void request_stop() noexcept {
    stop_.request_stop();
    if (current_)
      current_->external_host().request_stop();
    else if (poller_)
      poller_->shared()->external_host().request_stop();
  }
  template <class Predicate>
  void drive_until(Predicate done) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 同步驱动运行时");
    if (current_)
      current_->drive_until(std::move(done));
    else if (poller_)
      poller_->shared()->external_host().wait_until(std::move(done));
    else
      throw std::logic_error("runtime_context 已停止");
  }
  void run_entry(detached_task& root, task_tracker& tracker) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 同步运行入口");
    ::faio::detail::scoped_task_context submission{{.stop_token = stop_.get_token()}};
    start_detached(std::move(root), scheduler(), &tracker, task_lifetime());
    if (current_)
      current_->drive_until(tracker);
    tracker.wait();
  }

  /** @brief cancel_all 先请求根任务与 IO 取消，再排空；drain
   * 保留等待已有任务的语义。 */
  void stop(io::shutdown_policy policy = io::shutdown_policy::drain) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 同步关闭运行时");
    std::lock_guard stop_lock(stop_mutex_);
    if (stopped_.load(std::memory_order_acquire))
      return;
    external_host().begin_quiescing();  // 拒绝未 start 顶层 reservation 时仍保持 accepting。
    accepting_.store(false, std::memory_order_release);
    external_host().begin_draining();
    if (policy == io::shutdown_policy::cancel_all)
      request_stop();
    if (current_)
      current_->stop(policy);
    if (poller_)
      poller_->shutdown(policy);
    // 显式 stop 后保留稳定 host 地址到 context 析构。并发 run 的等待栈可在
    // 逻辑 root 已退役以后安全退出；线程和原生资源此时已经关闭/排空。
    stopped_.store(true, std::memory_order_release);
  }

  void shutdown(io::shutdown_policy policy = io::shutdown_policy::cancel_all) { stop(policy); }

  template <class F>
  auto submit_blocking(F&& function) {
    auto& host = external_host();
    const bool same_host = ::faio::detail::current_external_host() == &host;
    auto root = same_host ? host.lease(::faio::detail::current_external_scope)
                          : ::faio::detail::external_work_host::root_handle{};
    if (!root && !running())
      throw std::logic_error("runtime_context 已停止");
    auto target = root ? scheduler_for_external_root(root) : scheduler();
    const bool inherit = same_host && ::faio::detail::current_scheduler() == target;
    auto lifetime = root ? task_lifetime_for_external_root(root) : task_lifetime();
    auto& pool = current_ ? current_->blocking() : poller_->shared()->blocking();
    return start_blocking(pool,
                          std::forward<F>(function),
                          inherit ? ::faio::detail::current_tracker : nullptr,
                          inherit ? ::faio::detail::current_stop_token : stop_.get_token(),
                          lifetime, target, &host);
  }

  template <class T>
  join_handle<T> spawn_observed(task<T> child) {
    ::faio::detail::scoped_task_context submission{{.stop_token = stop_.get_token()}};
    return join_handle<T>{::faio::detail::start_observed(
        scheduler(), std::move(child), nullptr, stop_.get_token(), task_lifetime())};
  }

  template <class T>
  void submit(task<T> child) {
    if (!running())
      throw std::logic_error("runtime_context 已停止");
    ::faio::detail::scoped_task_context submission{{.stop_token = stop_.get_token()}};
    auto root = runtime_spawn_coro(std::move(child), stop_.get_token());
    start_detached(std::move(root), scheduler(), nullptr, task_lifetime());
  }

  template <class T>
  T block_on(task<T> child) {
    if (!running())
      throw std::logic_error("runtime_context 已停止");
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 线程调用 block_on；请 co_await task");
    ::faio::detail::scoped_task_context submission{{.stop_token = stop_.get_token()}};
    result_slot<T> slot;
    task_tracker tracker;
    tracker.add();
    auto root = block_coro(std::move(child), &slot, &tracker, stop_.get_token());
    start_detached(std::move(root), scheduler(), &tracker, task_lifetime());
    if (current_)
      current_->drive_until(tracker);
    tracker.wait();
    return slot.get();
  }

  // 兼容旧 API；新的可 co_await 并发组合由 join 提供。
  template <class... Ts>
  std::tuple<Ts...> wait_all(task<Ts>... tasks) {
    if (!running())
      throw std::logic_error("runtime_context 已停止");
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 线程调用 wait_all；请 co_await join");
    ::faio::detail::scoped_task_context submission{{.stop_token = stop_.get_token()}};
    static_assert((!std::is_void_v<Ts> && ...), "wait_all 不支持 void；请使用 join");
    if constexpr (sizeof...(Ts) == 0)
      return {};
    else {
      auto slots = std::tuple<result_slot<Ts>...>{};
      task_tracker tracker;
      // 哨兵计数使已经提交的任务不会在启动阶段就把计数归零。
      tracker.add();
      try {
        submit_all(tracker, slots, std::index_sequence_for<Ts...>{}, std::move(tasks)...);
      } catch (...) {
        tracker.done();
        if (current_)
          current_->drive_until(tracker);
        tracker.wait();  // 槽位在栈上，必须等已经启动的任务退出再传播错误。
        throw;
      }
      tracker.done();
      if (current_)
        current_->drive_until(tracker);
      tracker.wait();
      return collect(slots, std::index_sequence_for<Ts...>{});
    }
  }

 private:
  template <class... Ts, std::size_t... Is>
  void submit_all(task_tracker& tracker,
                  std::tuple<result_slot<Ts>...>& slots,
                  std::index_sequence<Is...>,
                  task<Ts>... tasks) {
    (submit_one(tracker, std::get<Is>(slots), std::move(tasks)), ...);
  }

  template <class T>
  void submit_one(task_tracker& tracker, result_slot<T>& slot, task<T> child) {
    auto root = block_coro(std::move(child), &slot, &tracker, stop_.get_token());
    tracker.add();
    start_detached(std::move(root), scheduler(), &tracker, task_lifetime());
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
