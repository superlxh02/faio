#ifndef FAIO_DETAIL_RUNTIME_CORE_STATE_MACHINE_HPP
#define FAIO_DETAIL_RUNTIME_CORE_STATE_MACHINE_HPP

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace faio::runtime::detail {

// ThreadCounters —— 线程计数器
// 使用两个独立的原子变量分别追踪工作线程数和搜索线程数，
class ThreadCounters {
public:
  explicit ThreadCounters() = default;
  explicit ThreadCounters(std::size_t num_workers) : _working(num_workers) {}

  // 获取当前搜索线程数
  [[nodiscard]]
  std::size_t num_searching() const {
    return _searching.load(std::memory_order_seq_cst);
  }

  // 获取当前工作线程数
  [[nodiscard]]
  std::size_t num_working() const {
    return _working.load(std::memory_order_seq_cst);
  }

  // 原子增加搜索线程数
  void inc_num_searching() {
    _searching.fetch_add(1, std::memory_order_seq_cst);
  }

  // 原子减少搜索线程数，返回减少后是否为零（即该线程是最后一个搜索线程）
  [[nodiscard]]
  bool dec_num_searching() {
    auto prev = _searching.fetch_sub(1, std::memory_order_seq_cst);
    assert(prev > 0 && "搜索线程数不能减至负数");
    return prev == 1;
  }

  // 唤醒一个线程：工作线程数 +1，搜索线程数按参数增量更新
  void wake_up_one(std::size_t searching_inc) {
    _working.fetch_add(1, std::memory_order_seq_cst);
    if (searching_inc > 0) {
      _searching.fetch_add(searching_inc, std::memory_order_seq_cst);
    }
  }

  // 原子减少工作线程数；若该线程处于搜索状态，同时减少搜索线程数。
  //  返回减少后搜索线程数是否为零（即是否为最后一个搜索线程）。
  [[nodiscard]]
  bool dec_num_working(bool is_searching) {
    _working.fetch_sub(1, std::memory_order_seq_cst);
    if (is_searching) {
      auto prev = _searching.fetch_sub(1, std::memory_order_seq_cst);
      assert(prev > 0 && "搜索线程数不能减至负数");
      return prev == 1;
    }
    return false;
  }

private:
  std::atomic<std::size_t> _working{0};   // 工作线程数
  std::atomic<std::size_t> _searching{0}; // 搜索线程数
};

// StateMachine —— 状态机
// 协调线程池中线程的状态，维护休眠线程集合，并限制搜索线程数量以实现负载均衡。
class StateMachine {
public:
  explicit StateMachine(std::size_t num_workers)
      : _counters(num_workers), _num_workers(num_workers) {}

  // 检查是否需要唤醒线程。如果需要，返回被唤醒的线程 ID；否则返回 nullopt。
  // 采用双重检查锁模式，减少不必要的互斥锁竞争。
  [[nodiscard]]
  std::optional<std::size_t> worker_to_notify() {
    // 第一次检查（无锁）
    if (!should_wakeup()) {
      return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(_mutex);

    // 第二次检查（持锁）
    if (!should_wakeup()) {
      return std::nullopt;
    }

    // 没有休眠线程可唤醒
    if (_sleepers.empty()) {
      return std::nullopt;
    }

    // 更新计数器：工作数 +1，搜索数 +1
    _counters.wake_up_one(1);

    // 从休眠列表中取出一个线程
    auto worker_id = _sleepers.back();
    _sleepers.pop_back();

    return worker_id;
  }

  // 将线程标记为休眠状态，更新计数器并记录到休眠集合。
  // 返回减少后是否为最后一个搜索线程。
  [[nodiscard]]
  bool set_sleeping(std::size_t worker_id, bool is_searching) {
    std::lock_guard<std::mutex> lock(_mutex);

    bool is_last = _counters.dec_num_working(is_searching);

    // 将线程加入休眠列表
    _sleepers.push_back(worker_id);

    return is_last;
  }

  // 尝试将线程标记为搜索状态。
  // 为实现负载均衡，搜索线程数不得超过总线程数的一半。
  // 成功返回 true，否则返回 false。
  [[nodiscard]]
  bool set_searching() {
    // 限制搜索线程数量，避免过多线程争抢任务队列
    if (2 * _counters.num_searching() >= _num_workers) {
      return false;
    }
    _counters.inc_num_searching();
    return true;
  }

  // 取消线程的搜索状态，返回是否为最后一个搜索线程。
  [[nodiscard]]
  bool cancel_searching() {
    return _counters.dec_num_searching();
  }

  // 从休眠集合中移除指定线程，返回是否移除成功。
  bool cancel_sleeping(std::size_t worker_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = std::find(_sleepers.begin(), _sleepers.end(), worker_id);
    if (it != _sleepers.end()) {
      _sleepers.erase(it);
      return true;
    }
    return false;
  }

  // 检查线程是否在休眠集合中。
  [[nodiscard]]
  bool contains(std::size_t worker_id) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return std::find(_sleepers.begin(), _sleepers.end(), worker_id) !=
           _sleepers.end();
  }

private:
  // 判断是否需要唤醒线程：
  // 当没有搜索线程且存在空闲线程时，应该唤醒一个休眠线程。
  [[nodiscard]]
  bool should_wakeup() const {
    return _counters.num_searching() == 0 &&
           _counters.num_working() < _num_workers;
  }

private:
  ThreadCounters _counters{};           // 线程计数器
  std::size_t _num_workers;             // 线程池大小
  std::vector<std::size_t> _sleepers{}; // 休眠线程列表
  mutable std::mutex _mutex{};          // 保护休眠列表的互斥锁
};

} // namespace faio::runtime::detail

#endif // FAIO_DETAIL_RUNTIME_CORE_STATE_MACHINE_HPP
