/**
 * @file faio/detail/execution/blocking_executor.hpp
 * @brief 将同步调用交给隔离线程执行的协程桥接：有界阻塞执行服务。
 * @details 同步工作由独立阻塞服务执行；有界队列以错误返回背压。停止仅记录取消原因，已开始的借用型调用须真实返回后才发布完成并恢复协程。
 */
#pragma once

#include "faio/detail/common/error.hpp"
#include "faio/detail/common/move_only_function.hpp"
#include <cerrno>
#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace faio::execution {
/** @brief 原生后端辅助服务只有真实 fallback 发生时才启动应用线程。 */
enum class executor_startup { preheated, on_demand };

/** @brief 不依赖运行时类型的有界阻塞执行服务。
 * @details readiness 服务构造时预热；原生后端的非opcode辅助服务按首次任务启动。
 * queue_limit 限制等待执行的任务数，运行中的任务最多 limit 个。
 * min_threads 描述最低预热要求；本实现预热全部 limit 个线程，因此总满足它。
 * 已接受任务拥有完成责任；close 严格排空并等待线程退出，不能从池内调用。
 */
class blocking_executor {
 public:
  /** @brief 建立有界服务；按启动策略预热或仅在真实任务提交时启动线程。
   * @param limit 固定执行线程上限，运行中任务最多占用该数量的服务线程。
   * @param queue_limit 等待队列容量，不包括已经被线程取出的任务。
   * @param min_threads 有效最低线程要求，必须位于 1 到 limit 之间。
   * @param startup readiness 预热，native 非 opcode 服务按需启动。
   */
  explicit blocking_executor(std::size_t limit = 4,
                             std::size_t queue_limit = 4096,
                             std::size_t min_threads = 1,
                             executor_startup startup = executor_startup::preheated)
      : queue_limit_(queue_limit), thread_limit_(limit) {
    /// @brief 无工作线程或无队列的服务无法履行接受后完成的契约。
    if (!limit || !queue_limit || !min_threads || min_threads > limit)
      throw std::invalid_argument("阻塞执行器线程数/队列容量无效");
    /// @brief 在控制面预留线程容器容量，避免 submit 触发容器增长。
    threads_.reserve(limit);
    try {
      /// @brief 固定预热全部执行线程，磁盘慢调用不会占用协程 worker。
      if (startup == executor_startup::preheated)
        start_threads();
    } catch (...) {
      /// @brief 部分线程构造失败时先通知退出，再由 close 回收已启动的线程。
      close();
      throw;
    }
  }

  blocking_executor(const blocking_executor&) = delete;

  blocking_executor& operator=(const blocking_executor&) = delete;

  ~blocking_executor() { close(); }

  /** @brief 尝试接受任务；满载返回 EAGAIN，调用方不得在 worker 阻塞等待。
   * @details 小型 noexcept 捕获和已擦除任务只在接受时转移；其他捕获为避免
   * 锁内分配，仍在容量检查前构造拥有型包装，失败时参数可能已经被移动。
   */
  template <class F>
    requires std::invocable<std::decay_t<F>&> && std::constructible_from<std::decay_t<F>, F&&>
  expected<void> try_submit(F&& job) noexcept {
    try {
      using job_type = ::faio::move_only_function<void()>;
      if constexpr (std::same_as<std::decay_t<F>, job_type>
                    || (job_type::stores_inline<F>
                        && std::is_nothrow_constructible_v<std::decay_t<F>, F&&>))
        return enqueue(std::forward<F>(job));
      else {
        /// @brief 堆分配和可能抛出的用户构造均在锁外，保持短临界区。
        job_type owned{std::forward<F>(job)};
        return enqueue(std::move(owned));
      }
    } catch (const std::bad_alloc&) {
      /// @brief 分配失败没有接受任务；调用方可用错误完成等待者。
      return std::unexpected{make_error(ENOMEM)};
    } catch (...) {
      /// @brief noexcept 控制面将异常转换成明确系统错误。
      return std::unexpected{make_error(EIO)};
    }
  }

  /** @brief 停止接受任务，排空已接受任务，然后回收全部阻塞线程。 */
  void close() noexcept {
    /// @brief 生命周期管理者串行调用 close；析构可以重复调用它。
    std::vector<std::jthread> threads;
    {
      /// @brief 与提交和取任务使用同一锁，使 closed 发布与队列观察有序。
      std::lock_guard lock(mutex_);
      /// @brief 所有线程已经移走时直接返回，不重复等待。
      if (closed_)
        return;
      /// @brief 已提交任务仍留在 jobs 中，线程直到队列为空才退出。
      closed_ = true;
      /// @brief 在锁外 join，避免阻塞线程退出时反向等待同一队列锁。
      threads.swap(threads_);
    }
    /// @brief 唤醒所有空闲线程，让它们排空或观察退出状态。
    cv_.notify_all();
    /// @brief jthread 析构执行 join；这里不强制终止任何系统调用。
    threads.clear();
  }

  /// @brief 在队列锁内读取尚未领取的任务数，不把运行中的任务算作等待容量。
  [[nodiscard]] std::size_t queued() const noexcept {
    std::lock_guard lock(mutex_);
    return jobs_.size();
  }

  /// @brief 返回构造时固定的等待队列容量，不包括正在执行的任务。
  [[nodiscard]] std::size_t queue_limit() const noexcept { return queue_limit_; }

  /** @brief 实际已经启动的线程数；原生请求期间按需服务应保持零。 */
  [[nodiscard]] std::size_t started_threads() const noexcept {
    std::lock_guard lock(mutex_);
    return threads_.size();
  }

 private:
  template <class F>
  expected<void> enqueue(F&& job) {
    /// @brief 只判断容量与交接任务所有权；不在队列锁内执行用户工作。
    std::unique_lock lock(mutex_);
    if (closed_)
      return std::unexpected{make_error(ECANCELED)};
    if (jobs_.size() >= queue_limit_)
      return std::unexpected{make_error(EAGAIN)};
    /// @brief 原生 SQE 不调用这里；只有实际 fallback 才按需启动辅助服务。
    start_threads();
    jobs_.emplace_back(std::forward<F>(job));
    lock.unlock();
    cv_.notify_one();
    return {};
  }

  /** @brief 在构造控制面或队列锁内启动固定线程，失败前没有接受新job。
   * @details
   * 部分创建成功的线程仍由本服务拥有；下一次提交继续补齐，close会join。
   */
  void start_threads() {
    /// @brief 部分线程创建失败后已启动线程仍归服务拥有，下次提交只补齐缺少数量。
    while (threads_.size() < thread_limit_)
      /// @brief jthread 由容器持有并在 close 的锁外 join，禁止分离线程导致服务悬空。
      threads_.emplace_back([this] { run(); });
  }

  /** @brief 独立服务线程按 FIFO 领取任务；关闭时排空而非强制中断系统调用。 */
  void run() noexcept {
    for (;;) {
      /// @brief 领取后 job 在栈上独占任务责任；出队即释放等待容量。
      ::faio::move_only_function<void()> job;
      {
        /// @brief 条件变量只阻塞独立服务线程，永远不阻塞协程 worker。
        std::unique_lock lock(mutex_);
        /// @brief 谓词处理虚假唤醒；关闭时也必须继续领取已有任务。
        cv_.wait(lock, [this] { return closed_ || !jobs_.empty(); });
        /// @brief 只有已关闭且没有已接受任务时才退出。
        if (jobs_.empty())
          return;
        /// @brief FIFO 领取任务，避免后提交请求无限插队。
        job = std::move(jobs_.front());
        /// @brief 从队列删除包装后仍由 job 保持执行与完成责任，锁外才运行任务。
        jobs_.pop_front();
      }
      /// @brief 类型化 execute 包装捕获系统调用异常；裸任务也不能杀死服务线程。
      try {
        /// @brief 用户/系统调用只能在独立服务线程运行，不能在队列锁内或 IO worker
        /// 执行。
        job();
      } catch (...) {
        /// @brief 未包装的任务抛出异常时明确终止，不能吞掉异常并静默丢失完成责任。
        std::terminate();
      }
    }
  }

  const std::size_t queue_limit_;  ///< 构造后固定的等待容量，try_submit 满载明确返回 EAGAIN。
  const std::size_t thread_limit_;  ///< 固定执行线程上限，按需启动也不超出此配置。
  mutable std::mutex mutex_;  ///< 串行保护任务队列、线程拥有权与 closed 发布。
  std::condition_variable cv_;  ///< 只阻塞独立服务线程，谓词同时观察任务与关闭状态。
  std::deque<::faio::move_only_function<void()>> jobs_;  ///< 已接受尚未领取的拥有型 FIFO 任务。
  std::vector<std::jthread> threads_;  ///< 构造时预留容量，close 移出锁外统一 join。
  bool closed_{};  ///< 同锁内一次停止接受，已接受任务仍必须排空。
};

/** @brief 携带服务租约的轻量执行器引用；不依赖 worker 或 runtime TLS。 */
class blocking_executor_ref {
 public:
  blocking_executor_ref() = default;

  explicit blocking_executor_ref(std::shared_ptr<blocking_executor> service) noexcept
      : service_(std::move(service)) {}

  /** @brief 仅借用外部执行器；外部所有者必须覆盖全部已接受任务。 */
  explicit blocking_executor_ref(blocking_executor& service) noexcept : borrowed_(&service) {}

  /// @brief 尝试接受稳定请求，明确区分已接受、暂时满载与拒绝。
  template <class F>
    requires std::invocable<std::decay_t<F>&> && std::constructible_from<std::decay_t<F>, F&&>
  expected<void> try_submit(F&& job) const noexcept {
    auto* service = service_ ? service_.get() : borrowed_;
    if (!service)
      return std::unexpected{make_error(ECANCELED)};
    return service->try_submit(std::forward<F>(job));
  }

  explicit operator bool() const noexcept { return service_ || borrowed_; }

  /// @brief 在服务锁内读取已创建的阻塞线程数量。
  std::size_t started_threads() const noexcept {
    auto* service = service_ ? service_.get() : borrowed_;
    return service ? service->started_threads() : 0;
  }

 private:
  std::shared_ptr<blocking_executor> service_;  ///< 拥有型服务租约覆盖异步任务生命期。
  blocking_executor* borrowed_{};  ///< 外部借用仅用于显式外部执行器，其所有者负责保证存活。
};
}
