/**
 * @file faio/detail/runtime/multi_thread/scheduler/ready_queue.hpp
 * @brief 运行时调度、线程协调与定时驱动：全局与 SPMC 本地就绪队列。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
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
/// @brief 跨线程的全局就绪队列；短锁保护容器，原子非空提示用于快速检查。
/// 队列不拥有协程帧：接收句柄表示承担安排执行的责任，不负责 destroy。
class global_ready_queue {
 public:
  /// @brief 初始为空且允许提交；容器和互斥锁均直接作为成员存储。
  global_ready_queue() = default;

  /// @brief 队列负责唯一的句柄调度归属，禁止复制内部状态。
  global_ready_queue(const global_ready_queue&) = delete;

  global_ready_queue& operator=(const global_ready_queue&) = delete;

  /// @brief 查询关闭状态；实际投递仍在持锁后再次检查，避免关闭与提交交错。
  bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

  /// @brief 关闭入队入口；调用方必须先排空根任务，随后唤醒所有线程退出。
  void close() {
    /// @brief 取得全局队列短锁后重新核对真实容器，把精确出队与非空提示更新放在同一临界区。
    std::lock_guard lock(mutex_);
    closed_.store(true, std::memory_order_release);
  }

  /// @brief 精确查询长度属于诊断接口；调度热路径只读取非空提示。
  std::size_t size() const {
    /// @brief 取得全局队列短锁后重新核对真实容器，把精确出队与非空提示更新放在同一临界区。
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  /// @brief 空队列检查无需获取容器锁；入睡方仍须在登记后重新检查。
  bool empty() const noexcept { return !has_tasks_.load(std::memory_order_acquire); }

  /// @brief 单个投递：容器分配失败时没有接管句柄，成功后发布新的队列长度。
  void push_back(std::coroutine_handle<> task) {
    /// @brief 取得全局队列短锁后重新核对真实容器，把精确出队与非空提示更新放在同一临界区。
    std::lock_guard lock(mutex_);
    if (closed())
      throw std::logic_error("调度队列已关闭");
    /// @brief 只记录空到非空的转换，减少每次入队写共享提示造成的缓存争用。
    const auto was_empty = queue_.empty();
    /// @brief 容器成功追加后才接管调度责任；分配抛异常时调用方仍拥有本次句柄。
    queue_.push_back(task);
    /// @brief 只在空/非空转换时写提示，避免每个任务都使所有消费者缓存行失效。
    if (was_empty)
      /// @brief release 发布全局队列由空变为非空，消费者可先用提示避开空锁争用。
      has_tasks_.store(true, std::memory_order_release);
  }

  /// @brief 批量投递具有整体成功/失败语义，供本地溢出回滚使用。
  void push_back_batch(std::span<const std::coroutine_handle<>> tasks) {
    /// @brief 取得全局队列短锁后重新核对真实容器，把精确出队与非空提示更新放在同一临界区。
    std::lock_guard lock(mutex_);
    if (closed())
      throw std::logic_error("调度队列已关闭");
    /// @brief 保存批量提交前长度，异常时精确撤销本批已经追加的前缀。
    const auto original_size = queue_.size();
    try {
      /// @brief 逐个写入或提交当前批次，保持输入任务的 FIFO 顺序。
      for (auto task : tasks)
        /// @brief 容器成功追加后才接管调度责任；分配抛异常时调用方仍拥有本次句柄。
        queue_.push_back(task);
    } catch (...) {
      /// @brief 临界区内无人消费这批任务，删除成功追加的前缀即可完整回滚。
      while (queue_.size() != original_size)
        /// @brief 只撤销本次尚未交付消费者的批次前缀，保留提交前队列状态。
        queue_.pop_back();
      throw;
    }
    /// @brief 只有向原空队列成功加入非空批次时才需要发布非空提示。
    if (original_size == 0 && !tasks.empty())
      /// @brief release 发布全局队列由空变为非空，消费者可先用提示避开空锁争用。
      has_tasks_.store(true, std::memory_order_release);
  }

  /// @brief 取出一个任务；空检查用于避免空闲线程反复争抢同一把锁。
  std::optional<std::coroutine_handle<>> try_pop() {
    /// @brief 先读保守原子提示，已知为空时避免争抢全局队列锁。
    if (empty())
      return std::nullopt;
    /// @brief 取得全局队列短锁后重新核对真实容器，把精确出队与非空提示更新放在同一临界区。
    std::lock_guard lock(mutex_);
    /// @brief 取得锁后再次核对真实容器，提示检查与出队之间可能已有其他消费者取空。
    if (queue_.empty())
      return std::nullopt;
    /// @brief 在队列锁内复制最早就绪句柄，随后由当前调用者独占其本次调度责任。
    auto task = queue_.front();
    /// @brief 移除已领取任务；该队列不销毁对应协程帧。
    queue_.pop_front();
    /// @brief 取得锁后再次核对真实容器，提示检查与出队之间可能已有其他消费者取空。
    if (queue_.empty())
      /// @brief 在队列锁内确认已空后撤销提示，后续新提交会负责重新发布非空。
      has_tasks_.store(false, std::memory_order_release);
    return task;
  }

  /// @brief 向调用者提供的固定缓冲区填充任务；此批量路径不创建临时 vector。
  std::size_t try_pop_batch(std::span<std::coroutine_handle<>> output) {
    /// @brief 没有输出容量或全局提示为空时不获取队列锁，也不领取任务。
    if (output.empty() || empty())
      return 0;
    /// @brief 取得全局队列短锁后重新核对真实容器，把精确出队与非空提示更新放在同一临界区。
    std::lock_guard lock(mutex_);
    /// @brief 本批至多领取真实队列长度与调用方输出预算的较小值。
    const auto count = std::min(queue_.size(), output.size());
    for (std::size_t i = 0; i < count; ++i) {
      /// @brief 按 FIFO 顺序把当前队首句柄交给调用方提供的固定缓冲区。
      output[i] = queue_.front();
      /// @brief 移除已领取任务；该队列不销毁对应协程帧。
      queue_.pop_front();
    }
    /// @brief 取得锁后再次核对真实容器，提示检查与出队之间可能已有其他消费者取空。
    if (queue_.empty())
      /// @brief 在队列锁内确认已空后撤销提示，后续新提交会负责重新发布非空。
      has_tasks_.store(false, std::memory_order_release);
    return count;
  }

 private:
  mutable std::mutex mutex_;  ///< 保护容器和关闭时的入队线性化。
  std::deque<std::coroutine_handle<>> queue_;  ///< 外部提交与本地溢出的就绪句柄。
  std::atomic<bool> has_tasks_{false};  ///< 只在空/非空转换时更新，供无锁空检查。
  std::atomic<bool> closed_{false};  ///< 禁止后续投递的生命周期标识。
};

/// @brief 单生产者、本地消费者加远端窃取者的固定容量就绪队列。
/// head 高 32 位为受保护槽位起点，低 32 位为下一个未领取任务。
/// 窃取期间二者分离，生产者不得复用窃取者尚未读取的槽位。
/// 快速槽仅供所属线程访问；远端线程只能观察和窃取环形队列。
template <std::size_t capacity_value = 256>
class local_ready_queue {
  static_assert(capacity_value >= 2 && (capacity_value & (capacity_value - 1)) == 0);
  static_assert(capacity_value <= std::numeric_limits<std::uint32_t>::max() / 2);

 public:
  /// @brief 初始化固定存储和零索引，不为本地队列单独分配内存。
  local_ready_queue() = default;

  /// @brief 注册后地址必须稳定，禁止复制队列及其生产/窃取边界。
  local_ready_queue(const local_ready_queue&) = delete;

  local_ready_queue& operator=(const local_ready_queue&) = delete;

  /// @brief 查询固定容量，编译期常量便于批量缓冲区和取模内联。
  static constexpr std::size_t capacity() noexcept { return capacity_value; }

  /// @brief 查询可窃取任务数；跨线程观察仅作提示，窃取时通过 CAS 重新确认。
  std::size_t size() const noexcept {
    const auto head = head_.load(std::memory_order_acquire);
    const auto tail = tail_.load(std::memory_order_acquire);
    return std::min<std::size_t>(static_cast<std::uint32_t>(tail - unpack(head).second),
                                 capacity_value);
  }

  /// @brief 只查询环形队列，不读取属于线程私有状态的快速槽。
  bool empty() const noexcept { return size() == 0; }

  /// @brief 所属线程查询全部本地任务；快速槽不能由远端线程访问。
  bool empty_local() const noexcept { return !fast_task_ && empty(); }

  /// @brief 所属生产线程计算可复用槽位，必须包含窃取期间仍受保护的槽位。
  std::size_t remaining_capacity() const noexcept {
    const auto head = head_.load(std::memory_order_acquire);
    /// @brief 尾索引只由所属生产线程更新，读取本线程发布位置无需额外同步。
    const auto tail = tail_.load(std::memory_order_relaxed);
    return capacity_value - static_cast<std::uint32_t>(tail - unpack(head).first);
  }

  /// @brief 普通本地调度优先使用快速槽，旧快速任务进入可窃取队列。
  /// 返回 true 表示产生可窃取工作，调用方可以按休眠状态决定是否唤醒同伴。
  bool push_local(std::coroutine_handle<> task, global_ready_queue& global) {
    if (!fast_task_) {
      fast_task_ = task;
      return false;
    }
    /// @brief 先成功安置旧任务，再替换快速槽；异常时新任务仍归调用者所有。
    push_back(*fast_task_, global);
    fast_task_ = task;
    return true;
  }

  /** @brief 预算耗尽时把等待的私有任务降级到原 FIFO/全局发布协议。
   * @details 只由所属线程调用；先安置句柄再清私有槽，入队失败仍保持原任务归属。
   *          发布后同伴可以恢复该帧，因此后续只清调度元数据，不访问协程帧。
   * @return 成功转交任务返回 true；没有私有任务时不改变任何队列。
   */
  bool demote_fast(global_ready_queue& global) {
    /// @brief 没有等待的私有任务，无需发布或改变快槽连续计数。
    if (!fast_task_)
      return false;
    /// @brief 复用原容量/窃取/溢出/异常回滚，失败前不清私有槽。
    push_back(*fast_task_,
              global);
    /// @brief 发布已经成功；只归还 owner 私有归属，不解引用可能已恢复的帧。
    fast_task_.reset();

    /// @brief 后续新快槽任务不继承此次已结束的私有优先序列。
    fast_streak_ = 0;
    /// @brief 由本地调度器承担已有并行工作或全局转交的通知责任。
    return true;
  }

  /// @brief 定时器完成、快速槽溢出等按FIFO入环形队列，保留共享窃取和溢出协议。
  void push_back(std::coroutine_handle<> task, global_ready_queue& global) {
    for (;;) {
      /// @brief 取得共享消费边界快照，acquire 与此前发布的边界更新配对。
      auto head = head_.load(std::memory_order_acquire);
      /// @brief 分离仍禁止生产者复用的保护边界，以及消费者下一次可领取的位置。
      const auto [protected_head, local_head] = unpack(head);
      /// @brief 尾索引只由所属生产线程更新，读取本线程发布位置无需额外同步。
      const auto tail = tail_.load(std::memory_order_relaxed);
      /// @brief 容量按保护边界计算，窃取者尚未复制的槽位仍算已占用，不能过早覆盖。
      if (static_cast<std::uint32_t>(tail - protected_head) < capacity_value) {
        /// @brief 在尚未发布的空闲环形槽写入句柄，远端消费者此时不会读到新任务。
        tasks_[tail & mask] = task;
        /// @brief release 发布新尾部，使 acquire 观察者在领取任务前可见对应槽位内容。
        tail_.store(tail + 1, std::memory_order_release);
        /// @brief 只有所属线程发布新任务，false 因此能够证明本地 FIFO 为空。
        fifo_hint_ = true;
        return;
      }
      /// @brief 存在远端窃取的受保护区间时不再搬移旧任务，只转交新任务以避免覆盖来源。
      if (protected_head != local_head) {
        /// @brief 远端正在读取槽位，不能覆盖；仅把新任务交给全局队列。
        global.push_back(task);
        return;
      }
      /// @brief 尝试独占半队列并整体转交全局；若边界竞争失败，则重新读取快照重试。
      if (handle_overflow(task, head, global))
        return;
    }
  }

  /// @brief 所属线程批量发布到本地队列；调用前已计算空间，不创建临时容器。
  void push_back_batch(std::span<const std::coroutine_handle<>> tasks) noexcept {
    /// @brief 调用方必须先按保护边界计算批量空间，避免覆盖在途窃取尚未读完的槽位。
    assert(tasks.size() <= remaining_capacity());
    /// @brief 读取只有当前生产线程写入的尾部，批次准备完成前不向远端发布。
    auto tail = tail_.load(std::memory_order_relaxed);
    /// @brief 逐个写入或提交当前批次，保持输入任务的 FIFO 顺序。
    for (auto task : tasks)
      /// @brief 按单调索引写入未发布槽位，掩码只负责物理环形定位。
      tasks_[tail++ & mask] = task;
    /// @brief 所有句柄写完后一次 release 发布整个批次，远端不会看到半初始化批次。
    tail_.store(tail, std::memory_order_release);
    /// @brief 空批次不能凭空制造非空提示，只有真实发布任务才设置提示。
    if (!tasks.empty())
      /// @brief 所属线程发布了可窃取 FIFO 任务，后续快速槽公平性检查必须重新观察共享队列。
      fifo_hint_ = true;
  }

  /// @brief 本地获取下一任务，连续快速槽恢复达到上限后给 FIFO 队列一次机会。
  std::optional<std::coroutine_handle<>> try_pop_local(bool allow_fast = true) noexcept {
    /// @brief 所属线程私有结果，任何未取得任务路径都不沿用旧来源。
    last_pop_from_fast_ = false;
    /// @brief 仅当本次允许且私有快速槽确实有任务时，才考虑优先执行该线程私有任务。
    if (allow_fast && fast_task_) {
      /// @brief FIFO 已被所属线程确认为空时无需每 3 次再读共享 head/tail。
      /// 远端只能移除任务，所有新增路径都在所属线程把提示置真，不会漏掉公平性检查。
      if (!fifo_hint_) {
        /// @brief 本地确认FIFO为空，领取私有任务不增加共享槽标签。
        last_pop_from_fast_ = true;
        /// @brief 取走私有快速句柄并原子于所属线程清空可选槽，远端线程不访问此私有状态。
        return std::exchange(fast_task_, std::nullopt);
      }
      /// @brief 共享 FIFO 已有工作时，私有快速槽最多连续取得配置次数的优先机会。
      if (fast_streak_ < fast_streak_limit) {
        /// @brief 累计这次快速槽领取，用于与共享 FIFO 的轮转公平性，区别于协作轮询预算。
        ++fast_streak_;
        /// @brief 三次 fast 公平性与协作额度是两种独立约束。
        last_pop_from_fast_ = true;
        /// @brief 取走私有快速句柄并原子于所属线程清空可选槽，远端线程不访问此私有状态。
        return std::exchange(fast_task_, std::nullopt);
      }
    }
    /// @brief 结束本轮快速优先序列，让后续 FIFO 消费或新快速链从公平性起点重新计数。
    fast_streak_ = 0;
    /// @brief 耗尽时先查真正FIFO；远端已偷空时必须继续检查私有槽。
    if (auto task = try_pop())
      return task;
    /// @brief 只有fast仍在时才记录该领取来源。
    last_pop_from_fast_ = fast_task_.has_value();
    /// @brief 仅fast不能死锁；执行入口会补新额度。
    return std::exchange(fast_task_,
                         std::nullopt);
  }

  /** @brief 仅所属线程读取最近一次成功本地领取来源，远端窃取不读写此字段。 */
  bool last_pop_from_fast() const noexcept { return last_pop_from_fast_; }

  /// @brief 所属线程消费环形队列；必须先读取槽位，再发布槽位可复用的 head。
  /// @return 成功领取的句柄；当前无可领取任务时返回 std::nullopt。
  /// @details CAS 是领取权的线性化点。失败时丢弃预读句柄并按最新边界重试，
  /// 不清空已释放槽位，避免与后续复用该槽的生产者发生写竞争。
  std::optional<std::coroutine_handle<>> try_pop() noexcept {
    /// @brief 取得共享消费边界快照，acquire 与此前发布的边界更新配对。
    auto head = head_.load(std::memory_order_acquire);
    /// @brief 消费边界可能与窃取者竞争，使用更新后的原子快照重试，直到独占领取或确认当前为空。
    for (;;) {
      /// @brief 分离仍禁止生产者复用的保护边界，以及消费者下一次可领取的位置。
      const auto [protected_head, local_head] = unpack(head);
      /// @brief acquire 读取已发布尾部；领取边界追上尾部即说明当前没有可消费任务。
      if (local_head == tail_.load(std::memory_order_acquire)) {
        /// @brief 所属线程确认空；远端不会再新增任何环形任务。
        fifo_hint_ = false;
        return std::nullopt;
      }
      /// @brief 在释放槽位保护之前先读出句柄；按容量掩码将单调索引映射到环形槽位。
      const auto task = tasks_[local_head & mask];
      /// @brief 无窃取者时同步推进两个边界；有窃取者时仅推进领取边界，继续保护其尚未复制的槽位。
      const auto next_head =
          pack(protected_head == local_head ? local_head + 1 : protected_head, local_head + 1);
      /// @brief CAS 竞争本次领取权；成功的 acq_rel 发布槽位可复用信息，失败则使用更新后的 head 重试。
      if (head_.compare_exchange_weak(
              head, next_head, std::memory_order_acq_rel, std::memory_order_acquire))
        /// @brief 只有成功推进领取边界的当前调用者才取得该句柄的本次执行责任。
        return task;
      /// @brief 槽位不清零；成功发布 head 后生产者可复用，之后写槽位会与生产者竞争。
    }
  }

  /// @brief 从本队列窃取一批到目标队列，并直接返回其中一个任务供窃取线程执行。
  /// 目标队列的所属线程调用本接口，远端不会写目标队列的 tail。
  /// @param destination 调用线程独占生产的目标本地队列，不能与来源相同。
  /// @return 本批直接执行的最后一个句柄；无法取得批次时返回 std::nullopt。
  /// @details 先分离来源的保护与领取边界，再复制任务，最后合并保护边界。
  /// 复制期间来源生产者不得复用被保护槽；目标仅在完整复制后发布剩余任务。
  std::optional<std::coroutine_handle<>> steal_into(local_ready_queue& destination) noexcept {
    /// @brief 禁止从自身窃取，避免来源与目标的生产边界互相覆盖。
    if (&destination == this)
      return std::nullopt;
    /// @brief 按目标队列的保护边界计算可用空间，不能覆盖仍被其他窃取者读取的目标槽位。
    const auto free_slots = destination.remaining_capacity();
    /// @brief 目标没有可复用槽位时不领取来源任务，保持双方队列不变。
    if (free_slots == 0)
      return std::nullopt;
    /// @brief 取得共享消费边界快照，acquire 与此前发布的边界更新配对。
    auto head = head_.load(std::memory_order_acquire);
    /// @brief 保存本次批量领取数量，成功锁定边界后用于复制及一次性发布。
    std::uint32_t count = 0;
    /// @brief 保存成功领取的来源起始索引，后续复制不再依赖变化的共享快照。
    std::uint32_t start = 0;
    /// @brief 按最新双边界重试本阶段 CAS，竞争失败不能沿用上一轮保护或领取快照。
    for (;;) {
      /// @brief 分离仍禁止生产者复用的保护边界，以及消费者下一次可领取的位置。
      const auto [protected_head, local_head] = unpack(head);
      /// @brief 两个边界分离表示已有窃取者在读槽位，本次不再建立第二个受保护区间。
      if (protected_head != local_head)
        return std::nullopt;
      /// @brief 用已发布尾索引减去领取边界，得到当前尚可领取的任务数量。
      const auto available =
          static_cast<std::uint32_t>(tail_.load(std::memory_order_acquire) - local_head);
      /// @brief 来源已被所属线程或其他窃取者取空，结束此次窃取。
      if (available == 0)
        return std::nullopt;
      /// @brief 两个原子快照跨越边界推进时，差值可能表现为过大无符号数，不能据此读取槽位。
      if (available > capacity_value) {
        /// @brief 快照跨越一次 head 推进，重新读取，避免把过期快照当成超大范围。
        head = head_.load(std::memory_order_acquire);
        continue;
      }
      /// @brief 最多领取向上取整的一半任务，同时受目标空闲槽位约束，兼顾来源继续执行与负载分摊。
      count = static_cast<std::uint32_t>(std::min<std::size_t>((available + 1) / 2, free_slots));
      /// @brief 在 CAS 前固定候选区间起点；若竞争失败，下一轮重新计算起点与数量。
      start = local_head;
      /// @brief 将领取边界推进到整批之后；保护边界暂不移动，从而独占本批并阻止生产者过早覆盖。
      if (head_.compare_exchange_weak(head,
                                      pack(start, start + count),
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire))
        break;
    }
    /// @brief 所有被领取槽位仍由 protected_head 保护，复制完成前生产者无法复用。
    const auto destination_tail = destination.tail_.load(std::memory_order_relaxed);
    /// @brief 依次处理已经独占的批次，索引按环形容量回绕。
    for (std::uint32_t i = 0; i < count; ++i)
      /// @brief 先将受保护的来源句柄复制到目标未发布槽位，远端此时尚不能看到目标新任务。
      destination.tasks_[(destination_tail + i) & mask] = tasks_[(start + i) & mask];
    /// @brief 保留批次最后一个任务由当前线程直接执行，其余任务才发布到目标环形队列。
    const auto immediate = destination.tasks_[(destination_tail + count - 1) & mask];
    /// @brief 复制完成后合并保护边界；本地消费者期间可能已继续推进 local_head。
    head = head_.load(std::memory_order_acquire);
    /// @brief 按最新双边界重试本阶段 CAS，竞争失败不能沿用上一轮保护或领取快照。
    for (;;) {
      /// @brief 以最新领取边界回收保护区间，避免覆盖所属线程在复制期间取得的消费进展。
      const auto local_head = unpack(head).second;
      /// @brief 将保护边界追上最新领取边界；CAS 失败则重新读取，绝不回退所属线程的消费进度。
      if (head_.compare_exchange_weak(head,
                                      pack(local_head, local_head),
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire))
        break;
    }
    /// @brief 返回的最后一个任务不入队，其余任务一次 release 发布给目标队列窃取者。
    destination.tail_.store(destination_tail + count - 1, std::memory_order_release);
    /// @brief 调用者正是 destination 的所属线程。
    if (count > 1)
      /// @brief 目标仍有已发布的 FIFO 任务，所属线程必须重新执行快速槽公平性检查。
      destination.fifo_hint_ = true;
    /// @brief 把直接执行项交给窃取线程，句柄发布后不再访问相应协程帧。
    return immediate;
  }

 private:
  /// @brief 暂时领取半队列，并把它与新任务一起交给全局队列；失败则恢复原 head。
  bool handle_overflow(std::coroutine_handle<> task,
                       std::uint64_t head,
                       global_ready_queue& global) {
    /// @brief 溢出转交从当前领取边界开始，来源槽位在全局接纳完成前继续受保护。
    const auto start = unpack(head).second;
    /// @brief 固定转移半个本地队列，既释放空间又保留一部分本地工作。
    constexpr auto count = static_cast<std::uint32_t>(capacity_value / 2);
    /// @brief 先分离保护与领取边界以独占待转移批次；竞争失败时不接管任何槽位，交回外层重新读取。
    if (!head_.compare_exchange_strong(
            head, pack(start, start + count), std::memory_order_acq_rel, std::memory_order_acquire))
      return false;
    /// @brief 固定栈批次容纳半队列加新任务，转交本身不再为临时批次数组分配内存。
    std::array<std::coroutine_handle<>, capacity_value / 2 + 1> batch;
    /// @brief 依次处理已经独占的批次，索引按环形容量回绕。
    for (std::uint32_t i = 0; i < count; ++i)
      /// @brief 在来源槽仍受保护期间复制本批句柄，转交全局失败时仍可恢复原消费边界。
      batch[i] = tasks_[(start + i) & mask];
    /// @brief 把新任务接在转移批次末尾，全局队列要么接纳整批，要么完全回滚。
    batch[count] = task;
    try {
      /// @brief 在全局短锁内原子地接纳整批句柄，成功后全局队列承担调度责任。
      global.push_back_batch(batch);
    } catch (...) {
      /// @brief 当前所属线程没有并发消费，远端窃取被分离 head 排除，可完整恢复。
      head_.store(pack(start, start), std::memory_order_release);
      throw;
    }
    /// @brief 全局接纳成功后同步推进保护与领取边界，生产者此后可以复用转移出的槽位。
    head_.store(pack(start + count, start + count), std::memory_order_release);
    return true;
  }

  /// @brief 高半部保护窃取者读取，低半部记录下一未领取任务；32 位差值按无符号回绕。
  /// @param protected_head 最早仍禁止生产者复用的槽位索引。
  /// @param local_head 下一条未被消费者或窃取者领取的任务索引。
  /// @return 可由一个 64 位原子操作整体替换的双边界状态。
  static std::uint64_t pack(std::uint32_t protected_head, std::uint32_t local_head) noexcept {
    /// @brief 先扩为 64 位再将保护边界放入高半部；低 32 位保存领取边界，两者可由一次 CAS 原子替换。
    return (static_cast<std::uint64_t>(protected_head) << 32) | local_head;
  }

  /// @brief 还原保护边界和本地消费边界，供 CAS 循环重新计算状态。
  /// @param head 一次原子读取取得的完整双边界状态。
  /// @return first 为保护边界，second 为下一未领取位置。
  static std::pair<std::uint32_t, std::uint32_t> unpack(std::uint64_t head) noexcept {
    /// @brief 右移提取槽位保护边界，截断提取下一未领取索引；索引差使用无符号回绕计算。
    return {static_cast<std::uint32_t>(head >> 32), static_cast<std::uint32_t>(head)};
  }

  static constexpr std::size_t mask = capacity_value - 1;  ///< 取模掩码，容量是 2 的幂。
  static constexpr unsigned fast_streak_limit =
      3;  ///< 连续快速槽执行上限，减少已有 FIFO 的等待轮数。
  std::array<std::coroutine_handle<>, capacity_value> tasks_{};  ///< 无额外分配的环形句柄存储。
  alignas(128) std::atomic<std::uint64_t> head_{
      0};  ///< 128B 覆盖常见 64/128B 缓存行；只改变布局，不改变队列或原子同步协议。 共享消费边界，远端 CAS 不与所属线程发布边界共享缓存行。
  alignas(128) std::atomic<std::uint32_t> tail_{0};  ///< 远端只读取此发布边界。
  alignas(128) std::optional<std::coroutine_handle<>> fast_task_;  ///< 私有快速槽独立于 tail，频繁恢复不使远端的 tail 缓存副本失效。
  bool fifo_hint_{false};  ///< 所属线程私有的保守非空提示，远端不读写。
  unsigned fast_streak_{0};  ///< 连续从快速槽取得任务的次数。
  bool last_pop_from_fast_{false};  ///< 私有领取结果；不在环形任务槽中添加标签或原子。
};
}
#endif
