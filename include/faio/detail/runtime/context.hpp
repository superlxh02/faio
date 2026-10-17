/**
 * @file faio/detail/runtime/context.hpp
 * @brief 运行时调度、线程协调与定时驱动：上下文与服务入口。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
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
  std::optional<T> value;  ///< 持有当前结果值或快照，读取前须确认操作已完成。
  std::exception_ptr exception;  ///< 跨异步边界保存的异常，结果领取时重新抛出。

  /// @brief 若结果槽保存了异常，则在结果消费线程重新抛出。
  void get_error() const {
    if (exception)
      std::rethrow_exception(exception);
  }

  /// @brief 取得当前对象保存的值或借用句柄，是否转交所有权由返回类型决定。
  T get() {
    get_error();
    return std::move(value.value());
  }
};

template <>
struct result_slot<void> {
  std::exception_ptr exception;  ///< 跨异步边界保存的异常，结果领取时重新抛出。

  /// @brief 取得当前对象保存的值或借用句柄，是否转交所有权由返回类型决定。
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
  /// @brief cancel_all 的 sleep/yield 不应触发无人观察异常终止。
  }
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
  runtime::io_backend selected_io_backend() const noexcept { return io::detail::compiled_io_backend; }
#endif
  /// @brief 查询纯调度入口；停止后拒绝创建指向已销毁调度域的借用引用。
  scheduler_ref scheduler() {
    if (!accepting_.load(std::memory_order_acquire))
      throw std::logic_error("runtime_context 已停止接受任务");
    if (current_)
      return current_->scheduler();
    if (poller_)
      return poller_->shared()->scheduler_reference();
    throw std::logic_error("runtime_context 已停止");
  }

  /// @brief 查询根任务计数服务，供外部提交入口与调度引用一起传给根帧。
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
  /// @brief 取得所属任务或服务的协作停止令牌。
  std::stop_token stop_token() const noexcept { return stop_.get_token(); }
  /// @brief 向当前任务及其受控后代传播协作停止请求。
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
  /// @brief 登记入口根任务并等待整个任务组排空，禁止在工作线程同步调用。
  void run_entry(detached_task& root, task_tracker& tracker) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 同步运行入口");
    /// @brief 在提交作用域安装运行时停止令牌，退出后恢复调用线程此前的任务上下文。
    ::faio::detail::scoped_task_context submission{{.stop_token = stop_.get_token()}};
    start_detached(std::move(root), scheduler(), &tracker, task_lifetime());
    if (current_)
      current_->drive_until(tracker);
    /// @brief 仅在外部同步线程等待任务组归零，保证栈上的结果与计数状态不再被异步借用。
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
    /// @brief 拒绝未 start 顶层 reservation 时仍保持 accepting。
    external_host().begin_quiescing();
    accepting_.store(false, std::memory_order_release);
    external_host().begin_draining();
    if (policy == io::shutdown_policy::cancel_all)
      request_stop();
    if (current_)
      current_->stop(policy);
    if (poller_)
      poller_->shutdown(policy);
    /// @brief 显式 stop 后保留稳定 host 地址到 context 析构。并发 run 的等待栈可在
    /// 逻辑 root 已退役以后安全退出；线程和原生资源此时已经关闭/排空。
    stopped_.store(true, std::memory_order_release);
  }

  /// @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。
  void shutdown(io::shutdown_policy policy = io::shutdown_policy::cancel_all) { stop(policy); }

  /// @brief 继承当前任务图归属，把同步调用移交运行时阻塞池并返回可观察句柄。
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

  /// @brief 提交子任务并返回唯一结果观察句柄，完成前共享状态保持存活。
  template <class T>
  join_handle<T> spawn_observed(task<T> child) {
    /// @brief 在提交作用域安装运行时停止令牌，退出后恢复调用线程此前的任务上下文。
    ::faio::detail::scoped_task_context submission{{.stop_token = stop_.get_token()}};
    return join_handle<T>{::faio::detail::start_observed(
        scheduler(), std::move(child), nullptr, stop_.get_token(), task_lifetime())};
  }

  /// @brief 消费用户任务所有权，并提交无需返回结果观察句柄的根任务。
  /// @param child 尚未启动的用户任务，成功提交后由根包装持有并执行。
  /// @throws std::logic_error 运行时已经停止接受新的顶层任务。
  /// @details 先构造惰性根包装，再登记生命周期计数并发布句柄；调度失败时
  /// 未转出的根帧负责清理。子任务继承运行时停止令牌，已有任务在关闭时排空。
  template <class T>
  void submit(task<T> child) {
    /// @brief 提交前验证运行时仍接纳顶层任务，已停止时不移交子任务帧。
    if (!running())
      throw std::logic_error("runtime_context 已停止");
    /// @brief 在提交作用域安装运行时停止令牌，退出后恢复调用线程此前的任务上下文。
    ::faio::detail::scoped_task_context submission{{.stop_token = stop_.get_token()}};
    /// @brief 创建惰性的根包装并接管子帧，使取消与未观察异常遵循运行时根任务策略。
    auto root = runtime_spawn_coro(std::move(child), stop_.get_token());
    /// @brief 先登记运行时根任务计数，再发布根句柄；调度失败时由根帧析构归还已登记责任。
    start_detached(std::move(root), scheduler(), nullptr, task_lifetime());
  }

  /// @brief 提交根包装并同步等待其任务组结束，随后领取结果或传播异常。
  template <class T>
  T block_on(task<T> child) {
    /// @brief 提交前验证运行时仍接纳顶层任务，已停止时不移交子任务帧。
    if (!running())
      throw std::logic_error("runtime_context 已停止");
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 线程调用 block_on；请 co_await task");
    /// @brief 在提交作用域安装运行时停止令牌，退出后恢复调用线程此前的任务上下文。
    ::faio::detail::scoped_task_context submission{{.stop_token = stop_.get_token()}};
    /// @brief 在同步调用栈上保存结果槽；必须等根任务真实完成后才允许该栈退出。
    result_slot<T> slot;
    /// @brief 建立本次同步等待专用的任务计数器，派生任务也必须在返回前排空。
    task_tracker tracker;
    /// @brief 先增加启动哨兵或根任务计数，防止任务快速完成造成提前归零。
    tracker.add();
    auto root = block_coro(std::move(child), &slot, &tracker, stop_.get_token());
    start_detached(std::move(root), scheduler(), &tracker, task_lifetime());
    if (current_)
      current_->drive_until(tracker);
    /// @brief 仅在外部同步线程等待任务组归零，保证栈上的结果与计数状态不再被异步借用。
    tracker.wait();
    /// @brief 确认全部任务退出后领取结果，若子任务失败则在同步调用方重新抛出异常。
    return slot.get();
  }

  /// @brief 兼容旧 API；新的可 co_await 并发组合由 join 提供。
  template <class... Ts>
  std::tuple<Ts...> wait_all(task<Ts>... tasks) {
    /// @brief 提交前验证运行时仍接纳顶层任务，已停止时不移交子任务帧。
    if (!running())
      throw std::logic_error("runtime_context 已停止");
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 线程调用 wait_all；请 co_await join");
    /// @brief 在提交作用域安装运行时停止令牌，退出后恢复调用线程此前的任务上下文。
    ::faio::detail::scoped_task_context submission{{.stop_token = stop_.get_token()}};
    static_assert((!std::is_void_v<Ts> && ...), "wait_all 不支持 void；请使用 join");
    if constexpr (sizeof...(Ts) == 0)
      return {};
    else {
      auto slots = std::tuple<result_slot<Ts>...>{};
      /// @brief 建立本次同步等待专用的任务计数器，派生任务也必须在返回前排空。
      task_tracker tracker;
      /// @brief 哨兵计数使已经提交的任务不会在启动阶段就把计数归零。
      tracker.add();
      try {
        submit_all(tracker, slots, std::index_sequence_for<Ts...>{}, std::move(tasks)...);
      } catch (...) {
        tracker.done();
        if (current_)
          current_->drive_until(tracker);
        /// @brief 槽位在栈上，必须等已经启动的任务退出再传播错误。
        tracker.wait();
        throw;
      }
      tracker.done();
      if (current_)
        current_->drive_until(tracker);
      /// @brief 仅在外部同步线程等待任务组归零，保证栈上的结果与计数状态不再被异步借用。
      tracker.wait();
      return collect(slots, std::index_sequence_for<Ts...>{});
    }
  }

 private:
  /// @brief 为输入任务依次建立根包装与结果槽关联，保持输入顺序。
  template <class... Ts, std::size_t... Is>
  void submit_all(task_tracker& tracker,
                  std::tuple<result_slot<Ts>...>& slots,
                  std::index_sequence<Is...>,
                  task<Ts>... tasks) {
    (submit_one(tracker, std::get<Is>(slots), std::move(tasks)), ...);
  }

  /// @brief 先登记计数再提交单个任务，提交失败时根帧负责配对归还。
  template <class T>
  void submit_one(task_tracker& tracker, result_slot<T>& slot, task<T> child) {
    auto root = block_coro(std::move(child), &slot, &tracker, stop_.get_token());
    /// @brief 先增加启动哨兵或根任务计数，防止任务快速完成造成提前归零。
    tracker.add();
    start_detached(std::move(root), scheduler(), &tracker, task_lifetime());
  }

  /// @brief 按输入下标移动领取已完成结果，异常在读取对应结果槽时传播。
  template <class... Ts, std::size_t... Is>
  static std::tuple<Ts...> collect(std::tuple<result_slot<Ts>...>& slots,
                                   std::index_sequence<Is...>) {
    return {std::get<Is>(slots).get()...};
  }

  std::stop_source stop_;  ///< 本操作或任务图的停止源，向已登记等待者广播协作取消。
  std::mutex stop_mutex_;  ///< 串行化运行时停止流程，防止重复销毁共享服务。
  std::atomic<bool> accepting_{true};  ///< 是否仍接受新的顶层任务，关闭入口先于排空已有任务。
  std::atomic<bool> stopped_{false};  ///< 停止状态的跨线程发布标志，后续接纳和查询据此拒绝新工作。
  runtime_config config_;  ///< 校验后的运行时配置，服务与调度预算据此初始化。
  std::unique_ptr<runtime_poller> poller_;  ///< 多线程运行时驱动器的唯一拥有者。
  std::unique_ptr<current_thread_runtime> current_;  ///< 当前线程运行时驱动器的唯一拥有者。
};
}
#endif
