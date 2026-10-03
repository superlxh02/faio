#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_POLLER_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_POLLER_HPP

#include "faio/detail/runtime/multi_thread/worker.hpp"
#include <cstddef>
#include <latch>
#include <thread>
#include <vector>

namespace faio::runtime::detail {
// 管理线程池及 shared 的生命周期；worker 对象直接存放在各工作线程的栈上。
class runtime_poller {
public:
  // 创建共享调度域，再启动并等待所有线程完成本地队列注册。
  explicit runtime_poller(const runtime_config &config)
      : shared_(config),
        workers_started_(static_cast<std::ptrdiff_t>(config._num_workers + 1)) {
    start_workers();
  }
  runtime_poller(const runtime_poller &) = delete;
  runtime_poller &operator=(const runtime_poller &) = delete;
  // 根帧全部销毁后关闭调度器；join 保证 shared 比任何本地调度状态活得更久。
  ~runtime_poller() { shutdown(io::shutdown_policy::drain); }
  /** @brief 保持 worker/调度器可运行直到取消完成、文件结果及关闭全部排空。 */
  void shutdown(io::shutdown_policy policy = io::shutdown_policy::cancel_all) {
    if (stopped_)
      return;
    if (policy == io::shutdown_policy::cancel_all)
      shared_.begin_io_shutdown(policy);
    shared_.wait_for_tasks();
    // 根任务可能产生无观察者用户 blocking job，结果发布者退出后才能关队列。
    shared_.blocking().close();
    shared_.begin_io_shutdown(policy);
    shared_.drain_io();
    close();
    wait_for_all();
    stopped_ = true;
  }
  // 借用共享组件，供 runtime_context 构造调度/生命周期引用。
  detail::shared *shared() noexcept { return &shared_; }
  // 外部线程等待所有 worker 析构完毕，包含本地注册注销和 I/O 资源释放。
  void wait_for_all() {
    for (auto &thread : threads_)
      if (thread.joinable())
        thread.join();
  }
  // 根任务排空后通知所有事件循环退出。
  void close() { shared_.close(); }

private:
  // 启动阶段先注册所有本地队列，再统一进入事件循环，窃取表在运行阶段保持稳定。
  void start_workers() {
    threads_.reserve(shared_.config()._num_workers);
    for (std::size_t i = 0; i < shared_.config()._num_workers; ++i) {
      threads_.emplace_back([this, i] {
        worker current_worker{shared_, i};
        workers_started_.arrive_and_wait();
        current_worker.run();
      });
    }
    workers_started_.arrive_and_wait();
  }

  detail::shared shared_; // 共享调度域、配置与根任务计数，先构造、最后销毁。
  std::latch workers_started_; // 全部本地队列注册完成的启动屏障，包含创建线程。
  bool stopped_{};
  std::vector<std::jthread>
      threads_; // 线程句柄；显式 join 后成员析构不再阻塞。
};
} // namespace faio::runtime::detail
#endif
