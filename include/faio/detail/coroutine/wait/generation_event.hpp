/**
 * @file faio/detail/coroutine/wait/generation_event.hpp
 * @brief 供协程等待状态变化的代际广播事件。
 */
#pragma once
#include "faio/detail/coroutine/wait/queue.hpp"
#include "faio/detail/coroutine/task.hpp"
#include <cstdint>
#include <memory>
#include <mutex>

namespace faio::detail {
/** @brief 在同一把锁下检查代际并登记等待者，避免检查后入队之间丢失唤醒。 */
template <class S = default_scheduler>
class BasicGenerationEvent {
  /** @brief 由事件与所有等待协程共同持有的广播状态。 */
  struct State {
    std::mutex mutex;            /**< 保护代际与等待队列，串行化检查、登记和广播。 */
    std::uint64_t generation{};  /**< 每次广播递增；与调用者的快照不同时无需等待。 */
    basic_wait_queue<S> waiters; /**< 尚未观察到新代际的协程等待节点。 */
  };

  /** @brief 将一次代际等待接入协程挂起与取消机制。 */
  struct Wait {
    std::shared_ptr<State> state; /**< 延长广播状态的生命周期，直到本次等待结束。 */
    std::uint64_t observed;       /**< 调用者开始等待前观察到的代际。 */
    basic_wait_node<S> node;      /**< 保存恢复句柄、停止回调和入队状态。 */

    /** @brief 统一在持锁的挂起阶段检查代际，避免无锁检查产生竞态。 */
    bool await_ready() const noexcept { return false; }

    /**
     * @brief 仅在代际未变化时登记等待，并接入当前协程的停止令牌。
     * @param handle 当前等待协程的恢复句柄。
     * @return 是否确实挂起；已收到广播或取消时继续执行。
     */
    template <class Promise>
      requires matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
      {
        std::lock_guard lock(state->mutex);
        if (state->generation != observed)
          return false;
        node.capture(handle);
        state->waiters.push(&node);
      }
      node.register_stop(state->mutex, state->waiters);
      return node.try_suspend();
    }

    /** @brief 恢复后将取消状态转换为统一的协程取消异常。 */
    void await_resume() {
      if (node.cancelled())
        throw operation_cancelled{};
    }
  };

 public:
  /** @brief 创建独立的广播状态；等待任务可在事件对象销毁后继续持有状态。 */
  BasicGenerationEvent() : state_(std::make_shared<State>()) {}

  /** @brief 禁止复制事件，避免把两个逻辑事件意外绑定到同一状态。 */
  BasicGenerationEvent(const BasicGenerationEvent&) = delete;
  /** @brief 禁止以赋值方式替换仍有等待者的事件。 */
  auto operator=(const BasicGenerationEvent&) -> BasicGenerationEvent& = delete;

  /**
   * @brief 取得后续 wait_change 使用的代际快照。
   * @return 当前广播代际。
   */
  auto snapshot() const noexcept -> std::uint64_t {
    std::lock_guard lock(state_->mutex);
    return state_->generation;
  }

  /**
   * @brief 等待代际与指定快照不同，或由当前任务的停止令牌取消。
   * @param observed 在检查受保护条件之前取得的代际快照。
   * @return 广播后完成的惰性任务；取消时抛出 operation_cancelled。
   */
  auto wait_change(std::uint64_t observed) -> task<void, S> { return wait_impl(state_, observed); }

  /** @brief 推进代际并恢复全部等待者；恢复动作在锁外执行。 */
  void notify_all() noexcept {
    basic_wait_node<S>* nodes;
    {
      std::lock_guard lock(state_->mutex);
      ++state_->generation;
      nodes = state_->waiters.take_all();
    }
    wake_all(nodes);
  }

 private:
  /**
   * @brief 将共享状态按值存入协程帧，避免任务借用事件对象。
   * @param state 本次等待拥有的广播状态。
   * @param observed 调用者观察到的代际。
   * @return 等待状态发生变化的任务。
   */
  static auto wait_impl(std::shared_ptr<State> state, std::uint64_t observed) -> task<void, S> {
    co_await Wait{std::move(state), observed, {}};
  }

  std::shared_ptr<State> state_; /**< 事件本身拥有的广播状态，与等待任务共享。 */
};

using GenerationEvent = BasicGenerationEvent<>;
}  // namespace faio::detail
