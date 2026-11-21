/**
 * @file faio/detail/runtime/multi_thread/scheduler/state_machine.hpp
 * @brief 工作、搜索和休眠状态机。
 */
#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_STATE_MACHINE_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_STATE_MACHINE_HPP

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace faio::runtime::detail {
/**
 * @brief 跟踪工作线程和搜索线程数量的计数器。
 * 使用两个独立原子变量跟踪工作/搜索线程；顺序一致性用于入睡与通知握手。
 * @details 将两个共享原子变量成对对齐；当目标平台的原子变量为 8 字节时，两者合计 16 字节，避免跨越 64 字节缓存行，
 *          不为整条缓存行增加填充，不改变计数协议或内存序。
 */
class alignas(2 * sizeof(std::atomic<std::size_t>)) worker_counters {
 public:
  /** @brief 构造工作与搜索线程计数器。 */
  explicit worker_counters() = default;

  /** @brief 构造工作与搜索线程计数器。 */
  explicit worker_counters(std::size_t num_workers) : working_(num_workers) {}

  /** @brief 获取当前搜索线程数 */
  [[nodiscard]]
  std::size_t num_searching() const {
    return searching_.load(std::memory_order_seq_cst);
  }

  /** @brief 获取当前工作线程数 */
  [[nodiscard]]
  std::size_t num_working() const {
    return working_.load(std::memory_order_seq_cst);
  }

  /** @brief 用 CAS 原子地检查并领取搜索名额，避免并发检查后同时增加导致超限。 */
  bool try_inc_searching(std::size_t limit) noexcept {
    auto searching = searching_.load(std::memory_order_seq_cst);
    while (searching < limit) {
      if (searching_.compare_exchange_weak(searching, searching + 1, std::memory_order_seq_cst))
        return true;
    }
    return false;
  }

  /** @brief 原子减少搜索线程数，返回减少后是否为零（即该线程是最后一个搜索线程） */
  [[nodiscard]]
  bool dec_num_searching() {
    auto prev = searching_.fetch_sub(1, std::memory_order_seq_cst);
    assert(prev > 0 && "搜索线程数不能减至负数");
    return prev == 1;
  }

  /** @brief 唤醒一个线程：工作线程数 +1，搜索线程数按参数增量更新 */
  void wake_up_one(std::size_t searching_inc) {
    working_.fetch_add(1, std::memory_order_seq_cst);
    if (searching_inc > 0) {
      searching_.fetch_add(searching_inc, std::memory_order_seq_cst);
    }
  }

  /**
   * @brief 原子减少工作线程数；若该线程处于搜索状态，同时减少搜索线程数。
   * 返回减少后搜索线程数是否为零（即是否为最后一个搜索线程）。
   */
  [[nodiscard]]
  bool dec_num_working(bool is_searching) {
    working_.fetch_sub(1, std::memory_order_seq_cst);
    if (is_searching) {
      auto prev = searching_.fetch_sub(1, std::memory_order_seq_cst);
      assert(prev > 0 && "搜索线程数不能减至负数");
      return prev == 1;
    }
    return false;
  }

 private:
  std::atomic<std::size_t> working_{0};   /**< 工作线程数 */
  std::atomic<std::size_t> searching_{0}; /**< 搜索线程数 */
};

/**
 * @brief 协调工作、搜索与休眠状态的调度状态机。
 * 维护休眠线程集合，并限制搜索线程数量以实现负载均衡。
 */
class scheduler_state_machine {
 public:
  /** @brief 构造运行时调度状态机。 */
  explicit scheduler_state_machine(std::size_t num_workers)
      : counters_(num_workers), num_workers_(num_workers) {
    sleepers_.reserve(num_workers);
  }

  /**
   * @brief 检查是否需要唤醒线程。如果需要，返回被唤醒的线程 ID；否则返回 nullopt。
   * 采用双重检查锁模式，减少不必要的互斥锁竞争。
   */
  [[nodiscard]]
  std::optional<std::size_t> worker_to_notify() {
    if (!should_wakeup()) {
      return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!should_wakeup()) {
      return std::nullopt;
    }

    if (sleepers_.empty()) {
      return std::nullopt;
    }

    counters_.wake_up_one(1);

    auto worker_id = sleepers_.back();
    sleepers_.pop_back();

    return worker_id;
  }

  /**
   * @brief 将线程标记为休眠状态，更新计数器并记录到休眠集合。
   * 返回减少后是否为最后一个搜索线程。
   */
  [[nodiscard]]
  bool set_sleeping(std::size_t worker_id, bool is_searching) {
    std::lock_guard<std::mutex> lock(mutex_);

    bool is_last = counters_.dec_num_working(is_searching);

    sleepers_.push_back(worker_id);

    return is_last;
  }

  /**
   * @brief 尝试将线程标记为搜索状态。
   * 为实现负载均衡，自主搜索名额为线程数的一半向上取整。
   * 成功返回 true，否则返回 false。
   */
  [[nodiscard]]
  bool set_searching() {
    return counters_.try_inc_searching((num_workers_ + 1) / 2);
  }

  /** @brief 取消线程的搜索状态，返回是否为最后一个搜索线程。 */
  [[nodiscard]]
  bool cancel_searching() {
    return counters_.dec_num_searching();
  }

  /** @brief 从休眠集合中移除指定线程，返回是否移除成功。 */
  bool cancel_sleeping(std::size_t worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find(sleepers_.begin(), sleepers_.end(), worker_id);
    if (it != sleepers_.end()) {
      sleepers_.erase(it);
      counters_.wake_up_one(0);
      return true;
    }
    return false;
  }

  /** @brief 外部执行通道不可窃取，必须唤醒持有该 FIFO 的指定工作线程。 */
  bool notify_worker(std::size_t worker_id) noexcept {
    std::lock_guard lock(mutex_);
    const auto it = std::find(sleepers_.begin(), sleepers_.end(), worker_id);
    if (it == sleepers_.end())
      return false;
    sleepers_.erase(it);
    counters_.wake_up_one(1);
    return true;
  }

  /** @brief 检查线程是否在休眠集合中。 */
  [[nodiscard]]
  bool contains(std::size_t worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(sleepers_.begin(), sleepers_.end(), worker_id) != sleepers_.end();
  }

 private:
  /**
   * @brief 没有搜索线程且存在空闲线程时，需要唤醒一个休眠线程。
   */
  [[nodiscard]]
  bool should_wakeup() const {
    return counters_.num_searching() == 0 && counters_.num_working() < num_workers_;
  }

 private:
  worker_counters counters_{};          /**< 线程计数器 */
  std::size_t num_workers_;             /**< 线程池大小 */
  std::vector<std::size_t> sleepers_{}; /**< 休眠线程列表 */
  mutable std::mutex mutex_{};          /**< 保护休眠列表的互斥锁 */
};
}  // namespace faio::runtime::detail

#endif
