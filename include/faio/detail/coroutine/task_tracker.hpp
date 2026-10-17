/**
 * @file faio/detail/coroutine/task_tracker.hpp
 * @brief 协程帧、任务上下文与并发组合：任务组完成计数。
 * @details 协程句柄的拥有、借用及转交由各类型明确约定。结果发布、停止传播和任务组计数须覆盖挂起、恢复及异常退出，发布恢复句柄后不再访问可能已销毁的帧。
 */
#ifndef FAIO_DETAIL_COROUTINE_TASK_TRACKER_HPP
#define FAIO_DETAIL_COROUTINE_TASK_TRACKER_HPP

#include <atomic>
#include <cstddef>
#include <utility>

namespace faio::detail {
/// @brief 一个执行作用域内尚未结束的根任务数量。只在根任务启动和完成时修改。
/// 作用域根任务计数器，通常由 block_on 的普通线程栈拥有。
/// 登记者必须在外层计数归零前完成所有派生任务登记，根帧结束再归还。
struct task_tracker {
  /// @brief 发布者所有权独立于借用的任务计数器；归零钩子回收帧后，发布者释放回调仍须有效。
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
    void* state_{};  ///< 类型擦除的借用状态地址，由配套静态回调还原真实类型。
    void (*release_)(void*) noexcept{};  ///< 归还借用状态或独立发布租约的静态适配回调。
  };

  /// @brief 添加根任务前只安装一次；回调所有者须在每对 begin/end 及生命周期服务的清理尾段内保持状态存活。
  void install_zero_completion_hook(void* state,
                                    void (*zero)(void*) noexcept,
                                    void (*begin)(void*) noexcept = nullptr,
                                    void (*end)(void*) noexcept = nullptr) noexcept {
    /// @brief 安装与归零及发布回调配套的借用状态，首次登记根任务之后不得再修改。
    hook_state_ = state;
    /// @brief 保存只允许触发一次的归零通知入口。
    zero_ = zero;
    /// @brief 保存开始独立结果发布责任的配对入口。
    begin_ = begin;
    /// @brief 保存归还独立结果发布责任的配对入口，必须在任务帧退役后仍有效。
    end_ = end;
  }
  std::atomic<std::size_t> pending{0};  ///< 尚未结束的根任务数量，不是普通嵌套 co_await 的子帧数量。

  /// @brief 投递根任务前登记一票；无需在增加计数时发布用户结果。
  void add() noexcept { pending.fetch_add(1, std::memory_order_relaxed); }

  /// @brief 根帧销毁时归还一票，最后一个完成者唤醒普通线程排空等待。
  completion_guard done() noexcept {
    /// @brief 递减计数前复制回调状态，归零钩子可能在当前调用返回前回收计数器所属帧。
    const auto state = hook_state_;
    /// @brief 先复制归零回调，计数发布后不再依赖原计数器对象存活。
    const auto zero = zero_;
    /// @brief 保存独立发布守卫的释放入口，后续回收不能让该入口随计数器一起失效。
    const auto end = end_;
    /// @brief 仅配置了独立发布钩子的任务图需要额外登记发布尾部责任。
    if (begin_)
      /// @brief 计数归零前先保留独立发布责任，防止归零清理越过当前发布尾部。
      begin_(state);
    /// @brief 以局部守卫持有独立发布责任，即使归零钩子回收所属帧也能安全归还。
    completion_guard publisher{state, end};
    /// @brief acq_rel 汇合任务完成写入；旧值为 1 时递减归零，通知所有阻塞等待者。
    if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      /// @brief 只由首次归零者取得归零钩子调用权，重复完成通知不能重复交付任务图终态。
      const bool call_zero = zero && !zero_fired_.exchange(true, std::memory_order_acq_rel);
      /// @brief 唤醒普通线程的原子等待者；各任务结果写入已由计数的 acquire/release 汇合。
      pending.notify_all();
      /// @brief 仅取得独占归零通知权时才调用可能回收任务图状态的用户态钩子。
      if (call_zero)
        /// @brief 通过预先保存的入口发布任务组归零，调用后不能再访问原计数器成员。
        zero(state);
    }
    return publisher;
  }

  /// @brief 普通线程等待任务数归零，不能用于阻塞 worker。
  void wait() const noexcept {
    /// @brief acquire 检查任务完成；已有任务全部结束时直接返回。
    for (auto n = pending.load(std::memory_order_acquire); n != 0;
         /// @brief 等待计数离开上次观测值，再复查到零，避免靠持续自旋占用 CPU。
         n = pending.load(std::memory_order_acquire))
      /// @brief 阻塞普通同步等待线程直到计数离开已观察值，醒来后仍需循环确认归零。
      pending.wait(n, std::memory_order_acquire);
  }

 private:
  void* hook_state_{};  ///< 归零回调使用的借用状态，必须覆盖回调执行期。
  void (*zero_)(void*) noexcept{};  ///< 根任务计数归零时调用的完成回调。
  void (*begin_)(void*) noexcept{};  ///< 开始一次结果发布时调用的配对回调。
  void (*end_)(void*) noexcept{};  ///< 完成一次结果发布时调用的配对回调，归还独立发布责任。
  std::atomic<bool> zero_fired_{false};  ///< 归零钩子是否已触发，防止重复交付终态。
};

/// @brief 当前用户协程所属的任务组。task 的 awaiter 在每次恢复用户代码前
/// 重建这个值，保证迁移到其他 worker 后 spawn 仍登记到正确的任务组。
/// 当前用户协程所属任务组的借用指针，无任务组时为空。
inline thread_local task_tracker* current_tracker{};
}
#endif
