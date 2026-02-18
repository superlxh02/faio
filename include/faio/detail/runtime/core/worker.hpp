#ifndef FAIO_DETAIL_RUNTIME_CORE_WORKER_HPP
#define FAIO_DETAIL_RUNTIME_CORE_WORKER_HPP

#include "faio/detail/common/util/rand.hpp"
#include "faio/detail/runtime/core/io_engine.hpp"
#include "faio/detail/runtime/core/queue.hpp"
#include "faio/detail/runtime/core/shared.hpp"
#include "fastlog/fastlog.hpp"
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
namespace faio::runtime::detail {

class Worker;
static inline thread_local Worker *current_worker{nullptr};

class Worker {
  friend class Shared;

public:
  Worker(Shared *shared, std::size_t worker_id)
      : _shared(shared), _worker_id(worker_id), _io_engine{shared->_config} {
    _shared->register_worker(this, worker_id);
    current_worker = this;
    current_shared = std::addressof(*shared);
  }
  ~Worker() {
    current_worker = nullptr;
    current_shared = nullptr;
    _shared->_shutdown_latch.arrive_and_wait();
  }

public:
  // worker运行的主函数
  // 逻辑：
  // 1.更新时间戳 2.周期性执行任务，更新线程关闭标志，驱动IO引擎处理IO 3.获取下一个任务
  // 4.窃取任务 5.处理IO 6.休眠
  void run() {
    while (!_is_shutdown) {
      // 更新时间戳
      tick();
      // 周期性执行任务，更新线程关闭标志，驱动IO引擎处理IO
      periodic();
      // 获取下一个任务
      if (auto task = get_next_task(); task) {
        excute(std::move(task.value()));
        continue;
      }
      // 窃取任务
      if (auto task = task_steal(); task) {
        excute(std::move(task.value()));
        continue;
      }
      // 处理IO
      if (drive_io()) {
        continue;
      }
      // 休眠
      sleep();
    }
    fastlog::console.debug("worker {} stop", _worker_id);
  }

public:
  // 唤醒worker
  void wake_up() { _io_engine.wake_up(); }

  // 将任务推送到本地队列
  // 如果存在缓存任务，则将旧缓存任务推送到本地队列，新任务替换缓存
  // 否则将任务缓存起来
  void push_back_task_to_local_queue(std::coroutine_handle<> task) {
    if (_task_cache.has_value()) {
      _local_queue.push_back(std::move(_task_cache.value()),
                             _shared->_global_queue);
      _task_cache = std::move(task);
      _shared->wake_up_one();
    } else {
      _task_cache.emplace(std::move(task));
    }
  }

private:
  // 周期性执行
  // 驱动IO引擎处理IO并且更新线程关闭标志
  void periodic() {
    if (this->_tick % _shared->_config._io_interval == 0) {
      drive_io();
      update_shutdown_flag();
    }
  }

  // 驱动IO引擎处理IO
  // 1.处理io  2.如果应该唤醒一个工作线程，则唤醒一个工作线程
  bool drive_io() {
    if (!_io_engine.drive(_local_queue, _shared->_global_queue)) {
      return false;
    }
    if (should_notify()) {
      _shared->wake_up_one();
    }
    return true;
  }
  // 休眠
  // 逻辑：
  // 1.更新线程关闭标志 2.设置休眠状态 3.等待IO引擎处理IO 4.更新线程关闭标志
  // 5.取消休眠状态 6.如果取消休眠状态成功，则退出循环
  void sleep() {
    update_shutdown_flag();
    if (set_sleeping()) {
      while (!_is_shutdown) {
        _io_engine.wait_and_drive(_local_queue, _shared->_global_queue);
        update_shutdown_flag();
        if (cancel_sleeping()) {
          fastlog::console.debug("worker {} break sleep", _worker_id);
          break;
        }
      }
    }
  }

  // 获取下一个任务
  std::optional<std::coroutine_handle<>> get_next_task() {
    // 如果时间戳是全局队列间隔的倍数，则从全局队列中获取下一个任务,否则从本地队列中获取下一个任务
    if (_tick % _shared->_config._global_queue_interval == 0) {
      return _shared->get_next_global_task().or_else(
          [this] { return get_next_local_task(); });
    } else {
      if (auto task = get_next_local_task(); task) {
        return task;
      }
      // 如果全局队列为空，则返回空
      if (_shared->_global_queue.empty()) {
        return std::nullopt;
      }
      // 获取到本地队列剩余大小的一半和容量一半的较小的那个
      auto num =
          std::min(_local_queue.remain_size(), _local_queue.capacity() / 2);
      if (num == 0) {
        return std::nullopt;
      }
      // 从全局队列获取num个任务
      auto tasks = _shared->_global_queue.try_pop_batch(num);
      if (tasks.has_value() && !tasks.value().empty()) {
        auto &task_vec = tasks.value();
        // 从全局队列获取的任务中拿到最后一个任务
        auto task = std::move(task_vec.back());
        // 从全局队列获取的任务中移除最后一个任务
        task_vec.pop_back();
        // 如果全局队列中还有任务，把它们放到本地队列中
        if (!task_vec.empty()) {
          _local_queue.push_back_batch(task_vec);
        }
        return task;
      } else {
        return std::nullopt;
      }
    }
  }

  // 窃取任务
  std::optional<std::coroutine_handle<>> task_steal() {
    // 设置搜索状态,如果是最后一个搜索线程,则返回空
    if (!set_searching()) {
      return std::nullopt;
    }
    // 轮询寻找负载最大的工作线程进行窃取
    std::size_t idx = 0, max_size = 0;
    for (auto &worker : _shared->_workers) {
      if (worker == this) {
        continue;
      }
      // 如果工作线程的本地队列大小大于目前最大大小,并且工作线程不在搜索状态,则更新最大大小和索引
      if (worker->_local_queue.size() > max_size && !worker->_is_searching) {
        idx = worker->_worker_id;
        max_size = worker->_local_queue.size();
      }
    }
    // 如果找到负载最大的工作线程,则窃取任务
    if (max_size > 0) {
      auto expected =
          _shared->_workers[idx]->_local_queue.be_stolen_by(_local_queue);
      return expected;
    }
    // 如果窃取失败,则从全局队列中获取任务
    return _shared->get_next_global_task();
  }

  // 以下接口都是关于线程状态切换和队列状态的
private:
  // 执行任务
  // 取消搜索状态，然后恢复任务
  void excute(std::coroutine_handle<> &&task) {
    this->cancel_searching();
    task.resume();
  }

  auto has_task() -> bool {
    return _task_cache.has_value() || !_local_queue.empty();
  }
  // 获取下一个本地任务
  std::optional<std::coroutine_handle<>> get_next_local_task() {
    // 如果存在任务缓存，则返回任务缓存
    if (_task_cache.has_value()) {
      std::optional<std::coroutine_handle<>> expected{std::nullopt};
      expected.swap(_task_cache);
      return expected;
    }
    // 否则从本地队列中获取下一个任务
    return _local_queue.try_pop();
  }
  // 更新线程关闭标志,根据全局队列是否关闭更新线程是否关闭标志
  void update_shutdown_flag() {
    if (!_is_shutdown) {
      _is_shutdown = _shared->_global_queue.closed();
    }
  }

  // 切换到休眠状态
  // 如果存在任务，则不切换
  // 如果不存在任务，切换成休眠状态并判断是不是最后一个搜索线程，如果是则唤醒一个可能正在等待的工作线程
  [[nodiscard]]
  bool set_sleeping() {
    if (has_task()) {
      return false;
    }
    // 切换到休眠状态，并返回是否为最后一个搜索线程
    auto is_last =
        _shared->_state_machine.set_sleeping(_worker_id, _is_searching);
    // 搜索状态标志位置为false
    _is_searching = false;

    // 如果为最后一个搜索线程，则唤醒一个可能正在等待的工作线程
    if (is_last) {
      _shared->wake_up_if_work_pending();
    }
    return true;
  }
  // 取消休眠状态
  // 如果本地有任务 或 全局队列有任务，从休眠集合中移除当前线程并退出 sleep。
  // 否则判断当前线程是否仍在休眠集合里，如果在则继续 sleep。
  [[nodiscard]]
  bool cancel_sleeping() {
    if (has_task() || !_shared->_global_queue.empty()) {
      _is_searching = !_shared->_state_machine.cancel_sleeping(_worker_id);
      return true;
    }

    if (_shared->_state_machine.contains(_worker_id)) {
      return false;
    }
    _is_searching = true;
    return true;
  }

  // 设置搜索状态
  [[nodiscard]]
  bool set_searching() {
    if (!_is_searching) {
      _is_searching = _shared->_state_machine.set_searching();
    }
    return _is_searching;
  }

  // 取消搜索状态
  // 如果当前线程不在搜索状态，则直接返回
  // 如果当前线程在搜索状态，则设置成非搜索状态
  // 如果当前线程是最后一个搜索线程，则唤醒一个工作线程
  void cancel_searching() {
    if (!_is_searching) {
      return;
    }
    _is_searching = false;
    if (_shared->_state_machine.cancel_searching()) {
      _shared->wake_up_one();
    }
  }

  // 应该唤醒当前线程吗
  [[nodiscard]]
  bool should_notify() const {
    if (_is_searching) {
      return false;
    }
    return _local_queue.size() > 1;
  }

private:
  // 计数器
  void tick() { _tick += 1; }

private:
  Shared *_shared;                                 // 共享资源指针
  std::size_t _worker_id;                          // 工作线程ID
  util::FastRand _rand{};                          // 随机数生成器
  std::uint32_t _tick{0};                          // 时间戳
  IOEngine _io_engine;                             // IO引擎
  LocalQueue<LOCAL_QUEUE_CAPACITY> _local_queue{}; // 本地队列
  std::optional<std::coroutine_handle<>> _task_cache{std::nullopt}; // 任务缓存
  bool _is_shutdown{false};                                         // 是否关闭
  std::atomic<bool> _is_searching{false}; // 是否在搜索
};

void Shared::wake_up_one() {
  if (auto idx = _state_machine.worker_to_notify(); idx) {
    _workers[idx.value()]->wake_up();
  }
}

void Shared::wake_up_all() {
  for (auto &worker : _workers) {
    worker->wake_up();
  }
}

void Shared::wake_up_if_work_pending() {
  // 检查本地队列或全局队列是否有待处理任务
  if (!_global_queue.empty()) {
    wake_up_one();
    return;
  }
  for (auto &worker : _workers) {
    if (!worker->_local_queue.empty()) {
      wake_up_one();
      return;
    }
  }
}

} // namespace faio::runtime::detail
#endif // FAIO_DETAIL_RUNTIME_CORE_WORKER_HPP