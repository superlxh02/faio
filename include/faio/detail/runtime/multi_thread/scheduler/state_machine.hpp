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
// worker_counters —— 线程计数器
// 使用两个独立原子变量跟踪工作/搜索线程；顺序一致性用于入睡与通知握手。
class worker_counters {
 public:
  explicit worker_counters() = default;

  explicit worker_counters(std::size_t num_workers) : working_(num_workers) {}

  // 获取当前搜索线程数
  [[nodiscard]]
  std::size_t num_searching() const {
    return searching_.load(std::memory_order_seq_cst);
  }

  // 获取当前工作线程数
  [[nodiscard]]
  std::size_t num_working() const {
    return working_.load(std::memory_order_seq_cst);
  }

  // 用 CAS 原子地检查并领取搜索名额，避免并发检查后同时增加导致超限。
  bool try_inc_searching(std::size_t limit) noexcept {
    auto searching = searching_.load(std::memory_order_seq_cst);
    while (searching < limit) {
      if (searching_.compare_exchange_weak(searching, searching + 1, std::memory_order_seq_cst))
        return true;
    }
    return false;
  }

  // 原子减少搜索线程数，返回减少后是否为零（即该线程是最后一个搜索线程）
  [[nodiscard]]
  bool dec_num_searching() {
    auto prev = searching_.fetch_sub(1, std::memory_order_seq_cst);
    assert(prev > 0 && "搜索线程数不能减至负数");
    return prev == 1;
  }

  // 唤醒一个线程：工作线程数 +1，搜索线程数按参数增量更新
  void wake_up_one(std::size_t searching_inc) {
    working_.fetch_add(1, std::memory_order_seq_cst);
    if (searching_inc > 0) {
      searching_.fetch_add(searching_inc, std::memory_order_seq_cst);
    }
  }

  // 原子减少工作线程数；若该线程处于搜索状态，同时减少搜索线程数。
  //  返回减少后搜索线程数是否为零（即是否为最后一个搜索线程）。
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
  std::atomic<std::size_t> working_{0};    // 工作线程数
  std::atomic<std::size_t> searching_{0};  // 搜索线程数
};

// scheduler_state_machine —— 状态机
// 协调线程池中线程的状态，维护休眠线程集合，并限制搜索线程数量以实现负载均衡。
class scheduler_state_machine {
 public:
  explicit scheduler_state_machine(std::size_t num_workers)
      : counters_(num_workers), num_workers_(num_workers) {
    // 所有休眠记录最多等于线程数，启动时预留，避免运行期间分配。
    sleepers_.reserve(num_workers);
  }

  // 检查是否需要唤醒线程。如果需要，返回被唤醒的线程 ID；否则返回 nullopt。
  // 采用双重检查锁模式，减少不必要的互斥锁竞争。
  [[nodiscard]]
  std::optional<std::size_t> worker_to_notify() {
    // 第一次检查（无锁）
    if (!should_wakeup()) {
      return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 第二次检查（持锁）
    if (!should_wakeup()) {
      return std::nullopt;
    }

    // 没有休眠线程可唤醒
    if (sleepers_.empty()) {
      return std::nullopt;
    }

    // 更新计数器：工作数 +1，搜索数 +1
    counters_.wake_up_one(1);

    // 从休眠列表中取出一个线程
    auto worker_id = sleepers_.back();
    sleepers_.pop_back();

    return worker_id;
  }

  // 将线程标记为休眠状态，更新计数器并记录到休眠集合。
  // 返回减少后是否为最后一个搜索线程。
  [[nodiscard]]
  bool set_sleeping(std::size_t worker_id, bool is_searching) {
    std::lock_guard<std::mutex> lock(mutex_);

    bool is_last = counters_.dec_num_working(is_searching);

    // 将线程加入休眠列表
    sleepers_.push_back(worker_id);

    return is_last;
  }

  // 尝试将线程标记为搜索状态。
  // 为实现负载均衡，自主搜索名额为线程数的一半向上取整。
  // 成功返回 true，否则返回 false。
  [[nodiscard]]
  bool set_searching() {
    // 限制搜索线程数量，避免过多线程争抢任务队列
    return counters_.try_inc_searching((num_workers_ + 1) / 2);
  }

  // 取消线程的搜索状态，返回是否为最后一个搜索线程。
  [[nodiscard]]
  bool cancel_searching() {
    return counters_.dec_num_searching();
  }

  // 从休眠集合中移除指定线程，返回是否移除成功。
  bool cancel_sleeping(std::size_t worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find(sleepers_.begin(), sleepers_.end(), worker_id);
    if (it != sleepers_.end()) {
      sleepers_.erase(it);
      // 自行醒来的 worker 原先在 set_sleeping 中已减少工作计数；
      // 若由 worker_to_notify 摘除，则那里已经同时更新工作与搜索计数。
      counters_.wake_up_one(0);
      return true;
    }
    return false;
  }

  // 检查线程是否在休眠集合中。
  [[nodiscard]]
  bool contains(std::size_t worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(sleepers_.begin(), sleepers_.end(), worker_id) != sleepers_.end();
  }

 private:
  // 判断是否需要唤醒线程：
  // 当没有搜索线程且存在空闲线程时，应该唤醒一个休眠线程。
  [[nodiscard]]
  bool should_wakeup() const {
    return counters_.num_searching() == 0 && counters_.num_working() < num_workers_;
  }

 private:
  worker_counters counters_{};           // 线程计数器
  std::size_t num_workers_;              // 线程池大小
  std::vector<std::size_t> sleepers_{};  // 休眠线程列表
  mutable std::mutex mutex_{};           // 保护休眠列表的互斥锁
};
}  // namespace faio::runtime::detail

#endif
