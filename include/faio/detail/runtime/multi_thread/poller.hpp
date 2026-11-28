/**
 * @file faio/detail/runtime/multi_thread/poller.hpp
 * @brief 工作线程组的启动与关闭。
 */
#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_POLLER_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_POLLER_HPP

#include "faio/detail/runtime/multi_thread/worker.hpp"
#include "faio/detail/runtime/common/worker_affinity.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <latch>
#include <optional>
#include <thread>
#include <vector>

namespace faio::runtime::detail {
/** @brief 运行时服务与调度上下文。 */
class runtime_context;

/** @brief 管理线程池及 shared 的生命周期；worker 对象直接存放在各工作线程的栈上。 */
class runtime_poller {
 public:
  /** @brief 创建共享调度域，再启动并等待所有线程完成本地队列注册。 */
  explicit runtime_poller(const runtime_config& config, runtime_context* context = nullptr)
      : shared_(config),
        workers_started_(static_cast<std::ptrdiff_t>(config._num_workers + 1)),
        startup_errors_(config._num_workers) {
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    shared_.external_host().set_context(context);
#endif
#if !defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) || !FAIO_EXPERIMENTAL_HAS_EXECUTION
    (void)context;
#endif
    start_workers();
  }

  /** @brief 禁止复制构造运行时 I/O 轮询器。 */
  runtime_poller(const runtime_poller&) = delete;

  /** @brief 禁止复制赋值。 */
  runtime_poller& operator=(const runtime_poller&) = delete;

  /** @brief 根帧全部销毁后关闭调度器；join 保证 shared 比任何本地调度状态活得更久。 */
  ~runtime_poller() { shutdown(io::shutdown_policy::DRAIN); }

  /**  @brief 保持工作线程和调度器可运行，直到取消、文件结果发布及关闭流程全部排空。 */
  void shutdown(io::shutdown_policy policy = io::shutdown_policy::CANCEL_ALL) {
    if (stopped_)
      return;
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    shared_.external_host().begin_quiescing();
    shared_.external_host().begin_draining();
    if (policy == io::shutdown_policy::CANCEL_ALL)
      shared_.external_host().request_stop();
#endif
    if (policy == io::shutdown_policy::CANCEL_ALL)
      shared_.begin_io_shutdown(policy);
    shared_.wait_for_tasks();
    shared_.blocking().close();
    shared_.begin_io_shutdown(policy);
    shared_.drain_io();
    close();
    wait_for_all();
    shared_.retire_frame_pool();
    shared_.completion_service().mark_stopped();
    stopped_ = true;
  }

  /** @brief 借用共享组件，供 runtime_context 构造调度/生命周期引用。 */
  detail::shared* shared() noexcept { return &shared_; }

  /** @brief 外部线程等待所有 worker 析构完毕，包含本地注册注销和 I/O 资源释放。 */
  void wait_for_all() {
    for (auto& thread : threads_)
      if (thread.joinable())
        thread.join();
  }

  /** @brief 根任务排空后通知所有事件循环退出。 */
  void close() { shared_.close(); }

 private:
  /** @brief 启动阶段先注册所有本地队列，再统一进入事件循环，窃取表在运行阶段保持稳定。 */
  void start_workers() {
    const auto count = shared_.config()._num_workers;
    threads_.reserve(count);
    std::size_t launched{};
    std::exception_ptr launch_error;
    try {
      for (; launched < count; ++launched) {
#if defined(FAIO_TEST_WORKER_AFFINITY)
        if (worker_affinity_test_fail_launch_after.load(std::memory_order_relaxed)
            == static_cast<std::ptrdiff_t>(launched))
          throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                  "injected worker thread creation failure");
#endif
        const auto index = launched;
        threads_.emplace_back([this, index] {
          std::optional<worker_affinity_scope> affinity;
          std::optional<worker> current_worker;
          try {
#if defined(FAIO_TEST_WORKER_AFFINITY)
            if (worker_affinity_test_fail_worker.load(std::memory_order_relaxed)
                == static_cast<std::ptrdiff_t>(index))
              throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                                      "injected worker affinity binding failure");
#endif
            const auto& cpus = shared_.config()._worker_cpus;
            if (!cpus.empty())
              affinity.emplace(cpus, index);
            current_worker.emplace(shared_, index);
          } catch (...) {
            startup_errors_[index] = std::current_exception();
            shared_.drop_unstarted_workers();
          }
          workers_started_.arrive_and_wait();
          start_gate_.wait();
          if (!startup_failed_.load(std::memory_order_acquire))
            current_worker->run();
        });
      }
    } catch (...) {
      launch_error = std::current_exception();
      const auto missing = static_cast<std::ptrdiff_t>(count - launched);
      shared_.drop_unstarted_workers(missing);
      workers_started_.count_down(missing);
    }
    workers_started_.arrive_and_wait();
    auto error = launch_error;
    for (const auto& candidate : startup_errors_)
      if (candidate && !error)
        error = candidate;
    startup_failed_.store(static_cast<bool>(error), std::memory_order_release);
    start_gate_.count_down();
    if (error) {
      wait_for_all();
      std::rethrow_exception(error);
    }
  }

  detail::shared shared_;      /**< 共享调度域、配置与根任务计数，先构造、最后销毁。 */
  std::latch workers_started_; /**< 全部本地队列注册完成的启动屏障，包含创建线程。 */
  std::latch start_gate_{1};   /**< 主线程收齐错误后单次发布启动决定。 */
  std::vector<std::exception_ptr> startup_errors_; /**< 每线程独占写入，启动屏障后读取。 */
  std::atomic<bool>
      startup_failed_{}; /**< 通过 release/acquire 显式发布，不进入工作线程执行任务的热路径。 */
  bool stopped_{};       /**< 标记线程组是否已关闭，避免重复停机。 */
  std::vector<std::jthread> threads_; /**< 线程句柄；显式 join 后成员析构不再阻塞。 */
};
}  // namespace faio::runtime::detail
#endif
