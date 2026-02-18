#ifndef FAIO_DETAIL_RUNTIME_CONTEXT_HPP
#define FAIO_DETAIL_RUNTIME_CONTEXT_HPP

#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/runtime/core/config.hpp"
#include "faio/detail/runtime/core/poller.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace faio::runtime::detail {

// ----------------------------------------------------------------------------
// completion_signal: 用于阻塞等待协程完成
// 基于 atomic + futex 风格的自旋+阻塞，高性能
// ----------------------------------------------------------------------------
enum class signal_status : std::uint8_t {
  pending = 0,
  ready = 1,
};

struct completion_signal {
  std::atomic<signal_status> status{signal_status::pending};

  void mark_ready() {
    status.store(signal_status::ready, std::memory_order::release);
    status.notify_all();
  }

  [[nodiscard]]
  bool is_ready() const noexcept {
    return status.load(std::memory_order::acquire) == signal_status::ready;
  }

  // 等待完成
  // 基于退避算法，先短自旋32次，再进入atomic::wait真正阻塞，直到mark_ready被调用。
  // 自旋的时候利用了CPU的pause指令，或者aarch64的yield指令优化性能。
  void wait() const noexcept {
    if (is_ready())
      return;
    for (int i = 0; i < 32; ++i) {
      if (is_ready())
        return;
#if defined(__x86_64__) || defined(_M_X64)
      __builtin_ia32_pause();
#elif defined(__aarch64__)
      asm volatile("yield");
#endif
    }
    while (!is_ready()) {
      status.wait(signal_status::pending, std::memory_order::acquire);
    }
  }
};

// ----------------------------------------------------------------------------
// block_on_tracker: 追踪 block_on/wait_all 上下文中的所有协程
// ----------------------------------------------------------------------------
struct block_on_tracker {
  std::atomic<std::size_t> pending_count{0};
  completion_signal completion;

  void register_subtask() {
    pending_count.fetch_add(1, std::memory_order::acq_rel);
  }

  void complete_subtask() {
    if (pending_count.fetch_sub(1, std::memory_order::acq_rel) == 1) {
      completion.mark_ready();
    }
  }

  void wait_all_done() { completion.wait(); }

  // 用于 task 的 completion_callback
  static void on_task_complete(void *arg) {
    auto *tracker = static_cast<block_on_tracker *>(arg);
    tracker->complete_subtask();
  }
};

// thread_local 当前 tracker（在 worker 线程上传播）
inline thread_local block_on_tracker *current_tracker{nullptr};

// ----------------------------------------------------------------------------
// result_slot<T>: 存放协程结果（值或异常）+ 完成信号
// 用于 block_on 和 wait_all 从 worker 线程传递结果到阻塞线程
// ----------------------------------------------------------------------------
template <typename T> struct result_slot {
  std::optional<T> value{std::nullopt};
  std::exception_ptr exception{nullptr};
  completion_signal signal;

  void set_value(T &&v) {
    value.emplace(std::move(v));
    signal.mark_ready();
  }

  void set_exception(std::exception_ptr e) {
    exception = std::move(e);
    signal.mark_ready();
  }

  T get() {
    signal.wait();
    if (exception)
      std::rethrow_exception(exception);
    return std::move(value.value());
  }

  // 用于 task 的 completion_callback：从 promise 提取结果写入 slot
  // arg 指向 pair<result_slot<T>*, coroutine_handle<task_promise<T>>>
  // 但这样需要堆分配 pair。改用不同方案。
};

template <> struct result_slot<void> {
  std::exception_ptr exception{nullptr};
  completion_signal signal;

  void set_value() { signal.mark_ready(); }

  void set_exception(std::exception_ptr e) {
    exception = std::move(e);
    signal.mark_ready();
  }

  void get() {
    signal.wait();
    if (exception)
      std::rethrow_exception(exception);
  }
};

// ----------------------------------------------------------------------------
// 外壳协程：用于 block_on 和 wait_all
//
// block_on/wait_all 需要拿到协程返回值。由于轻量 spawn 直接 push 裸 handle
// 无法传递返回值，block_on/wait_all 使用外壳协程来：
// 1) co_await 用户协程获取返回值
// 2) 将结果写入 result_slot
// 3) 通过 tracker 追踪完成
// ----------------------------------------------------------------------------

// block_on 外壳协程
template <typename T>
task<void> block_on_coro(task<T> t, result_slot<T> *slot,
                         block_on_tracker *tracker) {
  auto prev_tracker = current_tracker;
  current_tracker = tracker;
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(t);
      slot->set_value();
    } else {
      auto result = co_await std::move(t);
      slot->set_value(std::move(result));
    }
  } catch (...) {
    slot->set_exception(std::current_exception());
  }
  current_tracker = prev_tracker;
  tracker->complete_subtask(); // 主 task 完成
}

// wait_all 外壳协程
template <typename T>
task<void> wait_all_coro(task<T> t, result_slot<T> *slot,
                         block_on_tracker *tracker) {
  auto prev_tracker = current_tracker;
  current_tracker = tracker;
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(t);
      slot->set_value();
    } else {
      auto result = co_await std::move(t);
      slot->set_value(std::move(result));
    }
  } catch (...) {
    slot->set_exception(std::current_exception());
  }
  current_tracker = prev_tracker;
  tracker->complete_subtask();
}

// ============================================================================
// runtime_context
// ============================================================================
class runtime_context {
public:
  explicit runtime_context()
      : _config{},
        _poller{std::make_unique<runtime::detail::RuntimePoller>(_config)} {}

  explicit runtime_context(runtime::detail::Config config)
      : _config{config},
        _poller{std::make_unique<runtime::detail::RuntimePoller>(_config)} {}

  ~runtime_context() { stop(); }

  runtime_context(const runtime_context &) = delete;
  runtime_context &operator=(const runtime_context &) = delete;
  runtime_context(runtime_context &&) = delete;
  runtime_context &operator=(runtime_context &&) = delete;

public:
  [[nodiscard]]
  const runtime::detail::Config &config() const noexcept {
    return _config;
  }

  void stop() {
    if (_poller) {
      _poller.reset();
    }
  }

  [[nodiscard]]
  bool running() const noexcept {
    return _poller != nullptr;
  }

  // ============================================================================
  // spawn: 轻量提交协程到 runtime，零堆分配
  //
  // 直接 take() handle 并推送到队列。
  // 如果当前线程有 block_on tracker，自动注册追踪 + 设置完成回调。
  // worker 线程走本地队列（最快路径），其他线程走全局队列。
  // ============================================================================
  template <typename T> static void spawn(task<T> &&t) {
    auto handle = t.take();

    // 如果在 block_on 上下文中，注册 tracker 追踪
    if (auto *tracker = detail::current_tracker) {
      tracker->register_subtask();
      // 通过 promise 的 completion_callback 在协程结束时触发 complete_subtask
      auto &promise = handle.promise();
      promise._on_complete = &detail::block_on_tracker::on_task_complete;
      promise._on_complete_arg = tracker;
    }

    // 根据当前线程类型选择最优队列
    if (runtime::detail::current_worker != nullptr) {
      runtime::detail::push_task_to_local_queue(handle);
    } else {
      runtime::detail::push_task_to_global_queue(handle);
    }
  }

  // ============================================================================
  // block_on: 阻塞当前线程，等待 task 及其所有子 spawn 完成，返回 T
  //
  // 主协程直接在调用线程上 resume（不投递到队列），
  // 子 spawn 通过 tracker 追踪，全部完成后才解除阻塞。
  // ============================================================================
  template <typename T> auto block_on(task<T> t) -> T {
    detail::result_slot<T> slot;
    detail::block_on_tracker tracker;
    tracker.register_subtask(); // 主 task 占一个 pending

    // 构造外壳协程
    auto wrapper = detail::block_on_coro<T>(std::move(t), &slot, &tracker);
    auto handle = wrapper.take();

    // 主线程没有 io_uring 实例，不能直接 resume 协程
    // 将协程投递到全局队列，由 worker 线程执行
    runtime::detail::push_task_to_global_queue(handle);

    // 等待主协程 + 所有子 spawn 完成
    tracker.wait_all_done();

    return slot.get();
  }

  // ============================================================================
  // wait_all: 并行提交多个 task，阻塞等待全部完成，返回结果值
  //
  // 返回 std::tuple<T...>，直接拿到每个协程的返回值。
  // ============================================================================
  template <typename... Ts>
  auto wait_all(task<Ts>... tasks) -> std::tuple<Ts...> {
    detail::block_on_tracker tracker;

    // 预注册所有 task 的计数，防止竞态
    constexpr std::size_t N = sizeof...(Ts);
    tracker.pending_count.store(N, std::memory_order::relaxed);

    // 为每个 task 创建 result_slot 和外壳协程
    auto slots = std::make_unique<std::tuple<detail::result_slot<Ts>...>>();

    // 展开参数包，提交每个 task
    submit_wait_all_tasks<0>(tracker, *slots, std::move(tasks)...);

    // 等待全部完成
    tracker.wait_all_done();

    // 提取结果
    return extract_results(std::move(slots), std::index_sequence_for<Ts...>{});
  }

private:
  // wait_all 递归提交 helper
  template <std::size_t I, typename SlotsT, typename First, typename... Rest>
  void submit_wait_all_tasks(detail::block_on_tracker &tracker, SlotsT &slots,
                             task<First> first, task<Rest>... rest) {
    auto *slot = &std::get<I>(slots);
    auto wrapper =
        detail::wait_all_coro<First>(std::move(first), slot, &tracker);
    auto handle = wrapper.take();
    runtime::detail::push_task_to_global_queue(handle);

    if constexpr (sizeof...(Rest) > 0) {
      submit_wait_all_tasks<I + 1>(tracker, slots, std::move(rest)...);
    }
  }

  // wait_all 提取结果 helper
  template <typename... Ts, std::size_t... Is>
  static auto
  extract_results(std::unique_ptr<std::tuple<detail::result_slot<Ts>...>> slots,
                  std::index_sequence<Is...>) -> std::tuple<Ts...> {
    return std::tuple<Ts...>{std::get<Is>(*slots).get()...};
  }

private:
  runtime::detail::Config _config;
  std::unique_ptr<runtime::detail::RuntimePoller> _poller;
};

} // namespace faio::runtime::detail

#endif // FAIO_DETAIL_RUNTIME_CONTEXT_HPP
