/**
 * @file faio/detail/coroutine/wait/completion_event.hpp
 * @brief 一次性完成事件及等待链的原子发布实现。
 */
#ifndef FAIO_DETAIL_COROUTINE_WAIT_COMPLETION_EVENT_HPP
#define FAIO_DETAIL_COROUTINE_WAIT_COMPLETION_EVENT_HPP

#include "faio/detail/coroutine/wait/queue.hpp"
#include <cassert>
#include <utility>

namespace faio::detail {
/**
 * @brief 带记忆的一次性完成事件；完成等待不注册取消回调，也不从队列中撤回。
 * 节点只入队一次，notify 将整条链原子替换为永久完成标记，没有 ABA/回收重试。
 */
template <class S = default_scheduler>
class basic_completion_event {
 public:
  /** @brief 完成事件的协程等待器。 */
  struct awaiter {
    basic_completion_event& event; /**< 借用目标事件，所属任务排空后才销毁。 */
    basic_wait_node<S> node;       /**< 帧内节点，沿用登记/挂起交接协议。 */

    /** @brief 构造完成事件等待器。 */
    explicit awaiter(basic_completion_event& target) noexcept : event(target) {}

    /** @brief 构造完成事件等待器。 */
    awaiter(basic_completion_event& target, basic_resume_target<S> prepared) noexcept
        : event(target) {
      node.scheduler = std::move(prepared);
    }

    /** @brief 仅登记前移动；预留的控制槽随等待者转移，不重新申请容量。 */
    awaiter(awaiter&& other) noexcept : event(other.event) {
      assert(!other.node.handle && other.node.phase.load(std::memory_order_relaxed) == 0);
      node.scheduler = std::move(other.node.scheduler);
    }

    /** @brief 禁止复制完成事件等待器。 */
    awaiter(const awaiter&) = delete;
    /** @brief 禁止复制赋值。 */
    awaiter& operator=(const awaiter&) = delete;
    /** @brief 禁止移动赋值。 */
    awaiter& operator=(awaiter&&) = delete;

    /** @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。 */
    bool await_ready() const noexcept {
      return event.waiters_.load(std::memory_order_acquire) == &completed_;
    }

    /** @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。 */
    template <class Promise>
      requires matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      if (node.scheduler)
        node.handle = h;
      else
        node.capture(h);
      auto* head = event.waiters_.load(std::memory_order_acquire);
      for (;;) {
        if (head == &completed_)
          return false;
        node.next = head;
        if (event.waiters_.compare_exchange_weak(
                head, &node, std::memory_order_acq_rel, std::memory_order_acquire))
          break;
      }
      return node.try_suspend();
    }

    /** @brief 完成等待，不返回值。 */
    void await_resume() const noexcept {}
  };

  /** @brief 创建当前完成条件的协程等待器，具体取消语义由所属原语决定。 */
  awaiter wait() noexcept { return awaiter{*this}; }

  /** @brief 结构化任务在启动子任务前预留排空所需的容量，避免清理阶段因队列已满而失败。 */
  awaiter wait(basic_resume_target<S> prepared) noexcept {
    return awaiter{*this, std::move(prepared)};
  }

  /** @brief 发布带记忆的一次性完成，并转交所有已登记等待者的恢复责任。 */
  void notify() {
    auto* nodes = waiters_.exchange(&completed_, std::memory_order_acq_rel);
    if (nodes == &completed_)
      return;
    basic_wait_node<S>* ordered = nullptr;
    while (nodes) {
      auto* next = nodes->next;
      nodes->next = ordered;
      ordered = nodes;
      nodes = next;
    }
    wake_all(ordered);
  }

 private:
  inline static basic_wait_node<S>
      completed_{}; /**< 仅使用地址作为完成身份，从不访问或修改这个节点的内容。 */
  std::atomic<basic_wait_node<S>*> waiters_{
      nullptr}; /**< nullptr 空、节点链待完成、completed_ 已完成。 */
};

using completion_event = basic_completion_event<>;
}  // namespace faio::detail

#endif
