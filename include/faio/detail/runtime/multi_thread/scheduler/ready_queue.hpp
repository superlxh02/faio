/**
 * @file faio/detail/runtime/multi_thread/scheduler/ready_queue.hpp
 * @brief 全局与 SPMC 本地就绪队列。
 */
#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_READY_QUEUE_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_READY_QUEUE_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace faio::runtime::detail {
class current_thread_owner_scheduler;

/**
 * @brief 跨线程的全局就绪队列；短锁保护容器，原子非空提示用于快速检查。
 * 队列不拥有协程帧：接收句柄表示承担安排执行的责任，不负责 destroy。
 */
class global_ready_queue {
 public:
  /** @brief 初始为空且允许提交；容器和互斥锁均直接作为成员存储。 */
  global_ready_queue() = default;

  /** @brief 队列负责唯一的句柄调度归属，禁止复制内部状态。 */
  global_ready_queue(const global_ready_queue&) = delete;

  /** @brief 禁止复制赋值。 */
  global_ready_queue& operator=(const global_ready_queue&) = delete;

  /** @brief 查询关闭状态；实际投递仍在持锁后再次检查，避免关闭与提交交错。 */
  bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

  /** @brief 关闭入队入口；调用方必须先排空根任务，随后唤醒所有线程退出。 */
  void close() {
    std::lock_guard lock(mutex_);
    closed_.store(true, std::memory_order_release);
  }

  /** @brief 精确查询长度属于诊断接口；调度热路径只读取非空提示。 */
  std::size_t size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  /** @brief 空队列检查无需获取容器锁；入睡方仍须在登记后重新检查。 */
  bool empty() const noexcept { return !has_tasks_.load(std::memory_order_acquire); }

  /** @brief 单个投递：容器分配失败时没有接管句柄，成功后发布新的队列长度。 */
  void push_back(std::coroutine_handle<> task) {
    std::lock_guard lock(mutex_);
    if (closed())
      throw std::logic_error("调度队列已关闭");
    const auto was_empty = queue_.empty();
    queue_.push_back(task);
    if (was_empty)
      has_tasks_.store(true, std::memory_order_release);
  }

  /** @brief 批量投递具有整体成功/失败语义，供本地溢出回滚使用。 */
  void push_back_batch(std::span<const std::coroutine_handle<>> tasks) {
    std::lock_guard lock(mutex_);
    if (closed())
      throw std::logic_error("调度队列已关闭");
    const auto original_size = queue_.size();
    try {
      for (auto task : tasks)
        queue_.push_back(task);
    } catch (...) {
      while (queue_.size() != original_size)
        queue_.pop_back();
      throw;
    }
    if (original_size == 0 && !tasks.empty())
      has_tasks_.store(true, std::memory_order_release);
  }

  /** @brief 取出一个任务；空检查用于避免空闲线程反复争抢同一把锁。 */
  std::optional<std::coroutine_handle<>> try_pop() {
    if (empty())
      return std::nullopt;
    std::lock_guard lock(mutex_);
    if (queue_.empty())
      return std::nullopt;
    auto task = queue_.front();
    queue_.pop_front();
    if (queue_.empty())
      has_tasks_.store(false, std::memory_order_release);
    return task;
  }

  /** @brief 向调用者提供的固定缓冲区填充任务；此批量路径不创建临时 vector。 */
  std::size_t try_pop_batch(std::span<std::coroutine_handle<>> output) {
    if (output.empty() || empty())
      return 0;
    std::lock_guard lock(mutex_);
    const auto count = std::min(queue_.size(), output.size());
    for (std::size_t i = 0; i < count; ++i) {
      output[i] = queue_.front();
      queue_.pop_front();
    }
    if (queue_.empty())
      has_tasks_.store(false, std::memory_order_release);
    return count;
  }

 private:
  friend class current_thread_owner_scheduler;

  mutable std::mutex mutex_;                  /**< 保护容器和关闭时的入队线性化。 */
  std::deque<std::coroutine_handle<>> queue_; /**< 外部提交与本地溢出的就绪句柄。 */
  std::atomic<bool> has_tasks_{false};        /**< 只在空/非空转换时更新，供无锁空检查。 */
  std::atomic<bool> closed_{false};           /**< 禁止后续投递的生命周期标识。 */
};

/**
 * @brief 单生产者、本地消费者加远端窃取者的固定容量就绪队列。
 * head 高 32 位为受保护槽位起点，低 32 位为下一个未领取任务。
 * 窃取期间二者分离，生产者不得复用窃取者尚未读取的槽位。
 * 快速槽仅供所属线程访问；远端线程只能观察和窃取环形队列。
 */
template <std::size_t capacity_value = 256>
class local_ready_queue {
  static_assert(capacity_value >= 2 && (capacity_value & (capacity_value - 1)) == 0);
  static_assert(capacity_value <= std::numeric_limits<std::uint32_t>::max() / 2);

 public:
  /** @brief 初始化固定存储和零索引，不为本地队列单独分配内存。 */
  local_ready_queue() = default;

  /** @brief 注册后地址必须稳定，禁止复制队列及其生产/窃取边界。 */
  local_ready_queue(const local_ready_queue&) = delete;

  /** @brief 禁止复制赋值。 */
  local_ready_queue& operator=(const local_ready_queue&) = delete;

  /** @brief 查询固定容量，编译期常量便于批量缓冲区和取模内联。 */
  static constexpr std::size_t capacity() noexcept { return capacity_value; }

  /** @brief 查询可窃取任务数；跨线程观察仅作提示，窃取时通过 CAS 重新确认。 */
  std::size_t size() const noexcept {
    const auto head = head_.load(std::memory_order_acquire);
    const auto tail = tail_.load(std::memory_order_acquire);
    return std::min<std::size_t>(static_cast<std::uint32_t>(tail - unpack(head).second),
                                 capacity_value);
  }

  /** @brief 只查询环形队列，不读取属于线程私有状态的快速槽。 */
  bool empty() const noexcept { return size() == 0; }

  /** @brief 所属线程查询全部本地任务；快速槽不能由远端线程访问。 */
  bool empty_local() const noexcept { return !fast_task_ && empty(); }

  /** @brief 所属生产线程计算可复用槽位，必须包含窃取期间仍受保护的槽位。 */
  std::size_t remaining_capacity() const noexcept {
    const auto head = head_.load(std::memory_order_acquire);
    const auto tail = tail_.load(std::memory_order_relaxed);
    return capacity_value - static_cast<std::uint32_t>(tail - unpack(head).first);
  }

  /**
   * @brief 普通本地调度优先使用快速槽，旧快速任务进入可窃取队列。
   * 返回 true 表示产生可窃取工作，调用方可以按休眠状态决定是否唤醒同伴。
   */
  bool push_local(std::coroutine_handle<> task, global_ready_queue& global) {
    if (!fast_task_) {
      fast_task_ = task;
      return false;
    }
    push_back(*fast_task_, global);
    fast_task_ = task;
    return true;
  }

  /**
   * @brief 将旧快速槽和本批前缀一次发布到 FIFO，最后一项留在快速槽。
   * 仅由本队列所属 worker 调用，输入批次必须非空。
   * @return 空值表示容量不足，队列不变且不接管任何任务；否则整批接管，
   * 返回值表示是否增加了可窃取任务。
   */
  std::optional<bool> try_push_local_batch(
      std::span<const std::coroutine_handle<>> tasks) noexcept {
    assert(!tasks.empty());
    const auto fifo_count = tasks.size() - (fast_task_ ? 0u : 1u);
    if (fifo_count > remaining_capacity())
      return std::nullopt;
    auto tail = tail_.load(std::memory_order_relaxed);
    if (fast_task_)
      tasks_[tail++ & mask] = *fast_task_;
    for (auto task : tasks.first(tasks.size() - 1))
      tasks_[tail++ & mask] = task;
    fast_task_ = tasks.back();
    if (fifo_count != 0) {
      tail_.store(tail, std::memory_order_release);
      fifo_hint_ = true;
    }
    return fifo_count != 0;
  }

  /**
   *  @brief 预算耗尽时把等待的私有任务降级到原 FIFO/全局发布协议。
   * @details 只由所属线程调用；先安置句柄再清私有槽，入队失败仍保持原任务归属。
   *          发布后同伴可以恢复该帧，因此后续只清调度元数据，不访问协程帧。
   * @return 成功转交任务返回 true；没有私有任务时不改变任何队列。
   */
  bool demote_fast(global_ready_queue& global) {
    if (!fast_task_)
      return false;
    push_back(*fast_task_, global);
    fast_task_.reset();

    fast_streak_ = 0;
    return true;
  }

  /** @brief 定时器完成、快速槽溢出等任务按 FIFO 进入环形队列，沿用共享窃取和溢出协议。 */
  void push_back(std::coroutine_handle<> task, global_ready_queue& global) {
    for (;;) {
      auto head = head_.load(std::memory_order_acquire);
      const auto [protected_head, local_head] = unpack(head);
      const auto tail = tail_.load(std::memory_order_relaxed);
      if (static_cast<std::uint32_t>(tail - protected_head) < capacity_value) {
        tasks_[tail & mask] = task;
        tail_.store(tail + 1, std::memory_order_release);
        fifo_hint_ = true;
        return;
      }
      if (protected_head != local_head) {
        global.push_back(task);
        return;
      }
      if (handle_overflow(task, head, global))
        return;
    }
  }

  /** @brief 所属线程批量发布到本地队列；调用前已计算空间，不创建临时容器。 */
  void push_back_batch(std::span<const std::coroutine_handle<>> tasks) noexcept {
    assert(tasks.size() <= remaining_capacity());
    auto tail = tail_.load(std::memory_order_relaxed);
    for (auto task : tasks)
      tasks_[tail++ & mask] = task;
    tail_.store(tail, std::memory_order_release);
    if (!tasks.empty())
      fifo_hint_ = true;
  }

  /** @brief 本地获取下一任务，连续快速槽恢复达到上限后给 FIFO 队列一次机会。 */
  template <bool owner_only = false>
  std::optional<std::coroutine_handle<>> try_pop_local(bool allow_fast = true) noexcept {
    last_pop_from_fast_ = false;
    if (allow_fast && fast_task_) {
      if (!fifo_hint_) {
        last_pop_from_fast_ = true;
        return std::exchange(fast_task_, std::nullopt);
      }
      if (fast_streak_ < fast_streak_limit) {
        ++fast_streak_;
        last_pop_from_fast_ = true;
        return std::exchange(fast_task_, std::nullopt);
      }
    }
    fast_streak_ = 0;
    if constexpr (owner_only) {
      if (auto task = try_pop_owner())
        return task;
    } else if (auto task = try_pop()) {
      return task;
    }
    last_pop_from_fast_ = fast_task_.has_value();
    return std::exchange(fast_task_, std::nullopt);
  }

  /**  @brief 仅所属线程读取最近一次成功本地领取来源，远端窃取不读写此字段。 */
  bool last_pop_from_fast() const noexcept { return last_pop_from_fast_; }

  /**
   * @brief 所属线程消费环形队列；必须先读取槽位，再发布槽位可复用的 head。
   * @return 成功领取的句柄；当前无可领取任务时返回 std::nullopt。
   * @details CAS 是领取权的线性化点。失败时丢弃预读句柄并按最新边界重试，
   * 不清空已释放槽位，避免与后续复用该槽的生产者发生写竞争。
   */
  std::optional<std::coroutine_handle<>> try_pop() noexcept {
    auto head = head_.load(std::memory_order_acquire);
    for (;;) {
      const auto [protected_head, local_head] = unpack(head);
      if (local_head == tail_.load(std::memory_order_acquire)) {
        fifo_hint_ = false;
        return std::nullopt;
      }
      const auto task = tasks_[local_head & mask];
      const auto next_head =
          pack(protected_head == local_head ? local_head + 1 : protected_head, local_head + 1);
      if (head_.compare_exchange_weak(
              head, next_head, std::memory_order_acq_rel, std::memory_order_acquire))
        return task;
    }
  }

  /**
   * @brief 从本队列窃取一批到目标队列，并直接返回其中一个任务供窃取线程执行。
   * 目标队列的所属线程调用本接口，远端不会写目标队列的 tail。
   * @param destination 调用线程独占生产的目标本地队列，不能与来源相同。
   * @return 本批直接执行的最后一个句柄；无法取得批次时返回 std::nullopt。
   * @details 先分离来源的保护与领取边界，再复制任务，最后合并保护边界。
   * 复制期间来源生产者不得复用被保护槽；目标仅在完整复制后发布剩余任务。
   */
  std::optional<std::coroutine_handle<>> steal_into(local_ready_queue& destination) noexcept {
    if (&destination == this)
      return std::nullopt;
    const auto free_slots = destination.remaining_capacity();
    if (free_slots == 0)
      return std::nullopt;
    auto head = head_.load(std::memory_order_acquire);
    std::uint32_t count = 0;
    std::uint32_t start = 0;
    for (;;) {
      const auto [protected_head, local_head] = unpack(head);
      if (protected_head != local_head)
        return std::nullopt;
      const auto available =
          static_cast<std::uint32_t>(tail_.load(std::memory_order_acquire) - local_head);
      if (available == 0)
        return std::nullopt;
      if (available > capacity_value) {
        head = head_.load(std::memory_order_acquire);
        continue;
      }
      count = static_cast<std::uint32_t>(std::min<std::size_t>((available + 1) / 2, free_slots));
      start = local_head;
      if (head_.compare_exchange_weak(head,
                                      pack(start, start + count),
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire))
        break;
    }
    const auto destination_tail = destination.tail_.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < count; ++i)
      destination.tasks_[(destination_tail + i) & mask] = tasks_[(start + i) & mask];
    const auto immediate = destination.tasks_[(destination_tail + count - 1) & mask];
    head = head_.load(std::memory_order_acquire);
    for (;;) {
      const auto local_head = unpack(head).second;
      if (head_.compare_exchange_weak(head,
                                      pack(local_head, local_head),
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire))
        break;
    }
    destination.tail_.store(destination_tail + count - 1, std::memory_order_release);
    if (count > 1)
      destination.fifo_hint_ = true;
    return immediate;
  }

 private:
  friend class current_thread_owner_scheduler;

  /**
   * @brief 由唯一驱动线程领取 FIFO 队首。
   * @details 驱动线程独占两个索引，仍以 release 发布消费进度供远端观察。
   */
  std::optional<std::coroutine_handle<>> try_pop_owner() noexcept {
    const auto [protected_head, local_head] = unpack(head_.load(std::memory_order_relaxed));
    assert(protected_head == local_head);
    if (local_head == tail_.load(std::memory_order_relaxed)) {
      fifo_hint_ = false;
      return std::nullopt;
    }
    const auto task = tasks_[local_head & mask];
    const auto next = static_cast<std::uint32_t>(local_head + 1);
    head_.store(pack(next, next), std::memory_order_release);
    return task;
  }

  /** @brief 暂时领取半队列，并把它与新任务一起交给全局队列；失败则恢复原 head。 */
  bool handle_overflow(std::coroutine_handle<> task,
                       std::uint64_t head,
                       global_ready_queue& global) {
    const auto start = unpack(head).second;
    constexpr auto count = static_cast<std::uint32_t>(capacity_value / 2);
    if (!head_.compare_exchange_strong(
            head, pack(start, start + count), std::memory_order_acq_rel, std::memory_order_acquire))
      return false;
    std::array<std::coroutine_handle<>, capacity_value / 2 + 1> batch;
    for (std::uint32_t i = 0; i < count; ++i)
      batch[i] = tasks_[(start + i) & mask];
    batch[count] = task;
    try {
      global.push_back_batch(batch);
    } catch (...) {
      head_.store(pack(start, start), std::memory_order_release);
      throw;
    }
    head_.store(pack(start + count, start + count), std::memory_order_release);
    return true;
  }

  /**
   * @brief 高半部保护窃取者读取，低半部记录下一未领取任务；32 位差值按无符号回绕。
   * @param protected_head 最早仍禁止生产者复用的槽位索引。
   * @param local_head 下一条未被消费者或窃取者领取的任务索引。
   * @return 可由一个 64 位原子操作整体替换的双边界状态。
   */
  static std::uint64_t pack(std::uint32_t protected_head, std::uint32_t local_head) noexcept {
    return (static_cast<std::uint64_t>(protected_head) << 32) | local_head;
  }

  /**
   * @brief 还原保护边界和本地消费边界，供 CAS 循环重新计算状态。
   * @param head 一次原子读取取得的完整双边界状态。
   * @return first 为保护边界，second 为下一未领取位置。
   */
  static std::pair<std::uint32_t, std::uint32_t> unpack(std::uint64_t head) noexcept {
    return {static_cast<std::uint32_t>(head >> 32), static_cast<std::uint32_t>(head)};
  }

  static constexpr std::size_t mask = capacity_value - 1; /**< 取模掩码，容量是 2 的幂。 */
  static constexpr unsigned fast_streak_limit =
      3; /**< 连续快速槽执行上限，减少已有 FIFO 的等待轮数。 */
  std::array<std::coroutine_handle<>, capacity_value> tasks_{}; /**< 无额外分配的环形句柄存储。 */
  alignas(128) std::atomic<std::uint64_t> head_{
      0}; /**< 共享消费边界按 128 字节对齐，覆盖常见的 64 和 128 字节缓存行，避免远端 CAS 与所属线程的发布边界共用缓存行；仅调整布局，不改变队列或原子同步协议。 */
  alignas(128) std::atomic<std::uint32_t> tail_{0}; /**< 远端只读取此发布边界。 */
  alignas(128) std::optional<
      std::
          coroutine_handle<>> fast_task_; /**< 私有快速槽独立于 tail，频繁恢复不使远端的 tail 缓存副本失效。 */
  bool fifo_hint_{false};                 /**< 所属线程私有的保守非空提示，远端不读写。 */
  unsigned fast_streak_{0};               /**< 连续从快速槽取得任务的次数。 */
  bool last_pop_from_fast_{false}; /**< 私有领取结果；不在环形任务槽中添加标签或原子。 */
};
}  // namespace faio::runtime::detail
#endif
