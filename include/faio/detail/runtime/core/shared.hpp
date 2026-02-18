#ifndef FAIO_DETAIL_RUNTIME_CORE_SHARED_HPP
#define FAIO_DETAIL_RUNTIME_CORE_SHARED_HPP

#include "faio/detail/runtime/core/config.hpp"
#include "faio/detail/runtime/core/queue.hpp"
#include "faio/detail/runtime/core/state_machine.hpp"
#include <coroutine>
#include <cstddef>
#include <latch>
#include <optional>

namespace faio::runtime::detail {
class Worker;
class Shared;

static inline thread_local Shared *current_shared{nullptr};

// Shared 类，用于管理线程池中的共享资源
class Shared {
  friend class Worker;

public:
  Shared(const Config &config)
      : _config(config), _state_machine(config._num_workers),
        _shutdown_latch(static_cast<std::ptrdiff_t>(config._num_workers)) {
    current_shared = this;
    set_workers_size(config._num_workers);
  }
  ~Shared() { current_shared = nullptr; }

public:
  [[nodiscard]]
  const Config &config() const {
    return _config;
  }

  // 关闭全局队列并唤醒所有worker
  void close() {
    if (!_global_queue.closed()) {
      _global_queue.close();
      wake_up_all();
    }
  }

  // 注册工作线程
  void register_worker(Worker *worker, std::size_t worker_id) {
    _workers[worker_id] = worker;
  }

  // 设置工作线程数量,预留空间并初始化
  void set_workers_size(std::size_t size) {
    _workers.reserve(size);
    _workers.resize(size);
  }

  // 获取下一个全局任务
  [[nodiscard]]
  std::optional<std::coroutine_handle<>> get_next_global_task() {
    return _global_queue.try_pop();
  }

  // 单个线程任务推送到全局队列并唤醒一个worker
  // wake_up_one 通过 should_wakeup() 判断是否需要唤醒，当所有 worker
  // 都处于 working 状态时 should_wakeup() 返回 false。
  // 此时用 wake_up_all 通过 eventfd 唤醒所有休眠中的 worker 作为后备，
  // worker 的 cancel_sleeping 会检查全局队列并正确退出 sleep。
  void push_back_task_to_global_queue(std::coroutine_handle<> task) {
    _global_queue.push_back(task);
    wake_up_one();
    wake_up_all();
  }

  // 批量线程任务推送到全局队列并唤醒一个worker
  void push_back_batch_tasks_to_global_queue(
      std::span<std::coroutine_handle<>> tasks) {
    _global_queue.push_back_batch(tasks);
    wake_up_one();
  }

public:
  // 预留实现接口，由于shared不持有waker,所以需要在worker实现后实现以下接口
  void wake_up_one();
  void wake_up_all();
  void wake_up_if_work_pending();

private:
  const detail::Config _config;        // 配置
  detail::StateMachine _state_machine; // 状态机
  GlobalQueue _global_queue;           // 全局队列
  std::latch _shutdown_latch;          // 关闭latch
  std::vector<Worker *> _workers;      // 工作线程
};
} // namespace faio::runtime::detail
#endif // FAIO_DETAIL_RUNTIME_CORE_SHARED_HPP