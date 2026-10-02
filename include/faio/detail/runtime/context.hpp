#ifndef FAIO_DETAIL_RUNTIME_CONTEXT_HPP
#define FAIO_DETAIL_RUNTIME_CONTEXT_HPP

#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/runtime/core/config.hpp"
#include "faio/detail/coroutine/root_task.hpp"
#include "faio/detail/runtime/core/poller.hpp"
#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace faio::runtime::detail {
using ::faio::detail::detached_task;
using ::faio::detail::spawn_coro;
using ::faio::detail::start_detached;
using ::faio::detail::task_tracker;
using ::faio::detail::current_tracker;

template <class T> struct result_slot {
  std::optional<T> value;
  std::exception_ptr exception;
  void get_error() const { if (exception) std::rethrow_exception(exception); }
  T get() { get_error(); return std::move(value.value()); }
};
template <> struct result_slot<void> {
  std::exception_ptr exception;
  void get() { if (exception) std::rethrow_exception(exception); }
};

template <class T>
detached_task block_coro(task<T> child, result_slot<T>* slot,
                         task_tracker* tracker) {
  current_tracker = tracker;
  try {
    if constexpr (std::is_void_v<T>) co_await std::move(child);
    else slot->value.emplace(co_await std::move(child));
  } catch (...) {
    slot->exception = std::current_exception();
  }
  current_tracker = nullptr;
}

class runtime_context {
public:
  runtime_context() : runtime_context(runtime_config{}) {}
  explicit runtime_context(runtime_config config)
      : config_(validate_config(config)), poller_(std::make_unique<runtime_poller>(config_)) {}
  ~runtime_context() { stop(); }
  runtime_context(const runtime_context&) = delete;
  runtime_context& operator=(const runtime_context&) = delete;
  const runtime_config& config() const noexcept { return config_; }
  bool running() const noexcept { return !!poller_; }
  detail::shared* shared() noexcept { return poller_ ? poller_->shared() : nullptr; }
  // 查询纯调度入口；停止后拒绝创建指向已销毁调度域的借用引用。
  scheduler_ref scheduler() {
    if (!poller_) throw std::logic_error("runtime_context 已停止");
    return poller_->shared()->scheduler_reference();
  }
  // 查询根任务计数服务，供外部提交入口与调度引用一起传给根帧。
  task_lifetime_ref task_lifetime() {
    if (!poller_) throw std::logic_error("runtime_context 已停止");
    return poller_->shared()->task_lifetime_reference();
  }
  void stop() { poller_.reset(); }

  template <class T> static void spawn(task<T> child) {
    auto* tracker = current_tracker;
    auto root = spawn_coro(std::move(child), tracker);
    if (tracker) tracker->add();
    start_detached(std::move(root), ::faio::detail::current_scheduler(), tracker);
  }
  template <class T> void submit(task<T> child) {
    if (!poller_) throw std::logic_error("runtime_context 已停止");
    auto root = spawn_coro(std::move(child), nullptr);
    start_detached(std::move(root), scheduler(), nullptr, task_lifetime());
  }
  template <class T> T block_on(task<T> child) {
    if (!poller_) throw std::logic_error("runtime_context 已停止");
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 线程调用 block_on；请 co_await task");
    result_slot<T> slot;
    task_tracker tracker;
    tracker.add();
    auto root = block_coro(std::move(child), &slot, &tracker);
    start_detached(std::move(root), scheduler(), &tracker, task_lifetime());
    tracker.wait();
    return slot.get();
  }

  // 兼容旧 API；新的可 co_await 并发组合由 join 提供。
  template <class... Ts> std::tuple<Ts...> wait_all(task<Ts>... tasks) {
    if (!poller_) throw std::logic_error("runtime_context 已停止");
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 线程调用 wait_all；请 co_await join");
    static_assert((!std::is_void_v<Ts> && ...), "wait_all 不支持 void；请使用 join");
    if constexpr (sizeof...(Ts) == 0) return {};
    else {
      auto slots = std::tuple<result_slot<Ts>...>{};
      task_tracker tracker;
      // 哨兵计数使已经提交的任务不会在启动阶段就把计数归零。
      tracker.add();
      try {
        submit_all(tracker, slots, std::index_sequence_for<Ts...>{}, std::move(tasks)...);
      } catch (...) {
        tracker.done();
        tracker.wait(); // 槽位在栈上，必须等已经启动的任务退出再传播错误。
        throw;
      }
      tracker.done();
      tracker.wait();
      return collect(slots, std::index_sequence_for<Ts...>{});
    }
  }
private:
  template <class... Ts, std::size_t... Is>
  void submit_all(task_tracker& tracker, std::tuple<result_slot<Ts>...>& slots,
                  std::index_sequence<Is...>, task<Ts>... tasks) {
    (submit_one(tracker, std::get<Is>(slots), std::move(tasks)), ...);
  }
  template <class T>
  void submit_one(task_tracker& tracker, result_slot<T>& slot, task<T> child) {
    auto root = block_coro(std::move(child), &slot, &tracker);
    tracker.add();
    start_detached(std::move(root), scheduler(), &tracker, task_lifetime());
  }
  template <class... Ts, std::size_t... Is>
  static std::tuple<Ts...> collect(std::tuple<result_slot<Ts>...>& slots,
                                   std::index_sequence<Is...>) {
    return {std::get<Is>(slots).get()...};
  }
  runtime_config config_;
  std::unique_ptr<runtime_poller> poller_;
};

} // namespace faio::runtime::detail
#endif
