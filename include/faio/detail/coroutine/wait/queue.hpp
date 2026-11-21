/**
 * @file faio/detail/coroutine/wait/queue.hpp
 * @brief 帧内等待节点、侵入式等待队列及取消交接实现。
 */
#ifndef FAIO_DETAIL_COROUTINE_WAIT_QUEUE_HPP
#define FAIO_DETAIL_COROUTINE_WAIT_QUEUE_HPP

#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/runtime/common/coroutine_integration.hpp"
#include "faio/detail/coroutine/context/task_context.hpp"
#include "faio/detail/coroutine/scheduler/resume_target.hpp"
#include <atomic>
#include <concepts>
#include <coroutine>
#include <mutex>
#include <optional>
#include <stop_token>
#include <type_traits>

namespace faio::detail {
/** @brief 本库任务保留静态调度器类型约束；外部或根包装 promise 使用 TLS。 */
template <class S, class Promise>
concept matching_wait_scheduler =
    !has_task_scheduler_type<Promise> || std::same_as<S, typename Promise::scheduler_type>;

/** @brief 前置声明侵入式等待队列，取消回调只需保存其地址。 */
template <class S = default_scheduler>
struct basic_wait_queue;

/**
 * @brief 0=正在注册、1=允许异步恢复、2=正常唤醒、3=取消。通知先于交接到达时，
 * await_suspend 返回 false；交接成功后，通知/取消方负责安排恢复。
 * 一次等待操作的登记节点，由 awaiter/协程帧拥有，队列只借用指针。
 * 节点登记后不能移动或从外部销毁；必须由通知/取消完成交接，
 * 恢复等待操作后再清理 awaiter，不能仅凭 queued=false 就提前销毁。
 */
template <class S = default_scheduler>
struct basic_wait_node {
  std::coroutine_handle<> handle{};   /**< 等待完成后需要恢复的协程句柄，不拥有该协程帧。 */
  basic_resume_target<S> scheduler{}; /**< 登记时捕获调度域及恢复约束，通知线程不重读策略。 */
  basic_wait_node<S>* next{};         /**< 侵入式单链表的后继指针，仅用于等待队列链接。 */
  bool
      queued{}; /**< 标记节点是否仍由队列管理；通知与取消通过同一队列锁争夺摘除权，仅在所属 `basic_wait_queue<S>` 的 mutex 保护下访问。 */
  std::atomic<unsigned char> phase{
      0}; /**< 登记与挂起的交接状态：0 登记中，1 允许异步恢复，2 正常通知，3 取消；用于处理句柄已发布但 await_suspend 尚未返回时通知到达的竞态。 */

  /** @brief 停止源调用的回调，借用节点和队列设施；自身存储在节点的 callback 中。 */
  struct cancel_callback {
    basic_wait_node<S>* node;       /**< 本次等待的节点，回调不得在它生命周期结束后执行。 */
    std::mutex* mutex;              /**< 保护目标队列的外部互斥锁，正常通知和取消共用它。 */
    basic_wait_queue<S>* queue;     /**< 需要摘除本节点的等待队列，不拥有队列。 */
    std::atomic<bool>* has_waiters; /**< 可选的等待者提示标志，MPSC 用它跳过无等待者时的通知锁。 */

    /** @brief 停止被请求时执行：先摘除节点、更新提示，再发布取消并安排恢复。 */
    void operator()() const noexcept;
  };

  std::optional<std::stop_callback<cancel_callback>>
      callback; /**< 按需就地存储停止回调；销毁时注销回调并同步仍在执行的回调。 */

  /** @brief 创建尚未登记的空节点，所有状态使用成员初值。 */
  basic_wait_node() = default;

  /**
   * @brief 仅用于登记前的 awaiter 移动：创建新的空节点，不转移地址相关状态。
   * 已经捕获恢复目标或入队的节点不能通过此构造函数迁移。
   */
  basic_wait_node(basic_wait_node&&) noexcept {}

  /** @brief 禁止复制，否则一个等待操作可能出现两个节点身份。 */
  basic_wait_node(const basic_wait_node&) = delete;

  /** @brief 登记等待前捕获恢复目标；调用者随后在原语的队列锁下入队。 */
  template <class Promise>
    requires matching_wait_scheduler<S, Promise>
  void capture(std::coroutine_handle<Promise> h) {
    scheduler = capture_current_resume_target<S>();
    handle = h;
  }

  /**
   * @brief 为本次已入队等待注册当前停止令牌；必须在释放队列锁后调用。
   * stop_callback 构造可能同步执行回调，持锁构造会造成重复获取同一把锁。
   */
  void register_stop(std::mutex& mutex,
                     basic_wait_queue<S>& queue,
                     std::atomic<bool>* has_waiters = nullptr);

  /**
   * @brief 尝试完成等待登记到挂起的交接，返回值直接作为 await_suspend 的结果。
   * @pre 已发布节点并完成可选的停止回调注册；每次等待至多调用一次。
   * @return true 表示通知/取消方负责后续恢复；false 表示通知/取消已先到达，应继续执行。
   * 成功交接后，协程可能在本函数返回前恢复并销毁节点；
   * await_suspend 应直接返回，不得再访问节点或 awaiter。
   */
  [[nodiscard]] bool try_suspend() noexcept {
    unsigned char expected = 0;
    return phase.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
  }

  /** @brief 恢复后检查退出原因，具体原语据此抛 operation_cancelled。 */
  bool cancelled() const noexcept { return phase.load(std::memory_order_acquire) == 3; }

  /**
   * @brief 发布正常通知；调用者须已取得节点的唯一摘除权。
   * 调用后协程可能立刻恢复并销毁节点，通知方不能继续访问这个节点。
   */
  void wake() {
    auto previous = phase.exchange(2, std::memory_order_acq_rel);
    if (previous == 1)
      scheduler.schedule(handle);
  }

  /** @brief 发布取消；与 wake 一样要求已取得唯一摘除权，不强制销毁协程。 */
  void cancel() {
    auto previous = phase.exchange(3, std::memory_order_acq_rel);
    if (previous == 1)
      scheduler.schedule(handle);
  }
};

/**
 * @brief 不拥有节点的 FIFO 单链表；本类型不带锁，所有操作由使用方的锁保护。
 * push/pop 为 O(1)，取消或撤回时 remove 为 O(n)，只在争用慢路径使用。
 */
template <class S>
struct basic_wait_queue {
  basic_wait_node<S>* head{}; /**< 最早登记的等待者；空队列为 nullptr。 */
  basic_wait_node<S>* tail{}; /**< 最后登记的等待者，尾插时避免遍历整条链。 */

  /** @brief 将尚未入队的节点登记到队尾，调用时必须持有所属队列锁。 */
  void push(basic_wait_node<S>* node) noexcept {
    node->next = nullptr;
    node->queued = true;
    if (tail)
      tail->next = node;
    else
      head = node;
    tail = node;
  }

  /** @brief 摘除最早等待者并转交唯一通知权；不恢复协程，也不释放节点。 */
  basic_wait_node<S>* pop() noexcept {
    auto* node = head;
    if (node) {
      head = node->next;
      if (!head)
        tail = nullptr;
      node->next = nullptr;
      node->queued = false;
    }
    return node;
  }

  /** @brief 摘除整个队列并返回链表，批量通知者取得每个节点的唯一恢复权。 */
  basic_wait_node<S>* take_all() noexcept {
    auto* nodes = head;
    head = tail = nullptr;
    for (auto* node = nodes; node; node = node->next)
      node->queued = false;
    return nodes;
  }

  /** @brief 撤回指定节点，供取消和登记后复查使用；调用方必须持有队列锁。 */
  void remove(basic_wait_node<S>* node) noexcept {
    basic_wait_node<S>* previous = nullptr;
    for (auto* current = head; current; current = current->next) {
      if (current == node) {
        if (previous)
          previous->next = current->next;
        else
          head = current->next;
        if (tail == current)
          tail = previous;
        current->next = nullptr;
        current->queued = false;
        return;
      }
      previous = current;
    }
  }
};

/** @brief 实现停止回调注册；mutex、queue 和可选提示标志必须活到等待操作结束。 */
template <class S>
inline void basic_wait_node<S>::register_stop(std::mutex& mutex,
                                              basic_wait_queue<S>& queue,
                                              std::atomic<bool>* has_waiters) {
  const auto& token = ::faio::detail::current_task_context().stop_token;
  if (!token.stop_possible())
    return;
  callback.emplace(token, cancel_callback{this, &mutex, &queue, has_waiters});
}

/** @brief 停止请求到来时尝试接管仍在队列中的等待者。 */
template <class S>
inline void basic_wait_node<S>::cancel_callback::operator()() const noexcept {
  {
    std::lock_guard lock(*mutex);
    if (!node->queued)
      return;
    queue->remove(node);
    if (has_waiters)
      has_waiters->store(queue->head != nullptr, std::memory_order_seq_cst);
  }
  node->cancel();
}

/** @brief 正常恢复已经 take_all 摘除的节点链；调用方已释放队列锁。 */
template <class S>
inline void wake_all(basic_wait_node<S>* nodes) {
  while (nodes) {
    auto* next = nodes->next;
    nodes->wake();
    nodes = next;
  }
}

/** @brief 取消已经摘除的整条等待链，屏障破坏等场景使用。 */
template <class S>
inline void cancel_all(basic_wait_node<S>* nodes) {
  while (nodes) {
    auto* next = nodes->next;
    nodes->cancel();
    nodes = next;
  }
}

using wait_node = basic_wait_node<>;
using wait_queue = basic_wait_queue<>;
}  // namespace faio::detail

#endif
