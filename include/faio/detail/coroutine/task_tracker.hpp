#ifndef FAIO_DETAIL_COROUTINE_TASK_TRACKER_HPP
#define FAIO_DETAIL_COROUTINE_TASK_TRACKER_HPP

#include <atomic>
#include <cstddef>
#include <utility>

namespace faio::detail {
// 一个执行作用域内尚未结束的根任务数量。只在根任务启动和完成时修改。
// 作用域根任务计数器，通常由 block_on 的普通线程栈拥有。
// 登记者必须在外层计数归零前完成所有派生任务登记，根帧结束再归还。
struct task_tracker {
  // Publisher ownership is independent of this borrowed tracker. In particular,
  // its release callback must remain valid after the zero hook retires a frame.
  class completion_guard {
   public:
    completion_guard() noexcept = default;
    completion_guard(void* state, void (*release)(void*) noexcept) noexcept
        : state_(state), release_(release) {}
    completion_guard(const completion_guard&) = delete;
    completion_guard& operator=(const completion_guard&) = delete;
    completion_guard(completion_guard&& other) noexcept
        : state_(other.state_), release_(std::exchange(other.release_, nullptr)) {}
    ~completion_guard() { if (release_) release_(state_); }
   private:
    void* state_{};
    void (*release_)(void*) noexcept{};
  };

  // Install once before adding roots. The callback owner retains its state
  // through every begin/end pair, including the lifetime-service cleanup tail.
  void install_zero_completion_hook(void* state,
                                    void (*zero)(void*) noexcept,
                                    void (*begin)(void*) noexcept = nullptr,
                                    void (*end)(void*) noexcept = nullptr) noexcept {
    hook_state_ = state;
    zero_ = zero;
    begin_ = begin;
    end_ = end;
  }
  // 尚未结束的根任务数量，不是普通嵌套 co_await 的子帧数量。
  std::atomic<std::size_t> pending{0};

  // 投递根任务前登记一票；无需在增加计数时发布用户结果。
  void add() noexcept { pending.fetch_add(1, std::memory_order_relaxed); }

  // 根帧销毁时归还一票，最后一个完成者唤醒普通线程排空等待。
  completion_guard done() noexcept {
    const auto state = hook_state_;
    const auto zero = zero_;
    const auto end = end_;
    if (begin_)
      begin_(state);
    completion_guard publisher{state, end};
    // acq_rel 汇合任务完成写入；旧值为 1 时递减归零，通知所有阻塞等待者。
    if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      const bool call_zero = zero && !zero_fired_.exchange(true, std::memory_order_acq_rel);
      pending.notify_all();
      if (call_zero)
        zero(state);
    }
    return publisher;
  }

  // 普通线程等待任务数归零，不能用于阻塞 worker。
  void wait() const noexcept {
    // acquire 检查任务完成；已有任务全部结束时直接返回。
    for (auto n = pending.load(std::memory_order_acquire); n != 0;
         // 等待计数离开上次观测值，再复查到零，避免靠持续自旋占用 CPU。
         n = pending.load(std::memory_order_acquire))
      pending.wait(n, std::memory_order_acquire);
  }

 private:
  void* hook_state_{};
  void (*zero_)(void*) noexcept{};
  void (*begin_)(void*) noexcept{};
  void (*end_)(void*) noexcept{};
  std::atomic<bool> zero_fired_{false};
};

// 当前用户协程所属的任务组。task 的 awaiter 在每次恢复用户代码前
// 重建这个值，保证迁移到其他 worker 后 spawn 仍登记到正确的任务组。
// 当前用户协程所属任务组的借用指针，无任务组时为空。
inline thread_local task_tracker* current_tracker{};
}  // namespace faio::detail
#endif
