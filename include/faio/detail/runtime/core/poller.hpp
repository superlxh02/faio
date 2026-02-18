#ifndef FAIO_DETAIL_RUNTIME_CORE_POLLER_HPP
#define FAIO_DETAIL_RUNTIME_CORE_POLLER_HPP

#include "faio/detail/runtime/core/worker.hpp"
#include <thread>
namespace faio::runtime::detail {

class RuntimePoller {
public:
  RuntimePoller(const Config &config)
      : _shared(config), _sync_start(static_cast<std::ptrdiff_t>(
                             _shared.config()._num_workers + 1)) {
    work();
  }

  ~RuntimePoller() {
    close();
    wait_for_all();
  }

public:
  // 等待所有线程完成
  void wait_for_all() {
    // 等待所有 Worker 线程完成任务
    for (auto &worker : _runtime_thread_pool) {
      worker.join();
    }
  }
  // 关闭共享资源
  void close() { _shared.close(); }

private:
  // 工作函数，创建线程并运行工作者
  void work() {
    for (std::size_t i = 0; i < _shared.config()._num_workers; ++i) {
      _runtime_thread_pool.emplace_back([this, i]() {
        Worker worker{&_shared, i};
        // 等待所有的worker全部创建完成(shared内的worker数组完整注册好)
        _sync_start.arrive_and_wait();
        // 统一启动run
        worker.run();
      });
    }
    // 等待所有线程启动完成，此函数才执行完成
    _sync_start.arrive_and_wait();
  }

private:
  std::vector<std::jthread> _runtime_thread_pool;
  Shared _shared;
  std::latch _sync_start;
};

// 投递任务到当前worker实例的本地队列
static inline void push_task_to_local_queue(std::coroutine_handle<> task) {
  if (current_worker == nullptr) {
    throw std::runtime_error("current_worker is nullptr");
  }
  current_worker->push_back_task_to_local_queue(task);
}
// 投递任务到全局队列
static inline void push_task_to_global_queue(std::coroutine_handle<> task) {
  if (current_shared == nullptr) {
    throw std::runtime_error("current_shared is nullptr");
  }
  current_shared->push_back_task_to_global_queue(task);
}

// 投递批量任务到全局队列
static inline void
push_batch_tasks_to_global_queue(std::span<std::coroutine_handle<>> tasks) {
  if (current_shared == nullptr) {
    throw std::runtime_error("current_shared is nullptr");
  }
  current_shared->push_back_batch_tasks_to_global_queue(std::move(tasks));
}

} // namespace faio::runtime::detail
#endif // FAIO_DETAIL_RUNTIME_CORE_POLLER_HPP