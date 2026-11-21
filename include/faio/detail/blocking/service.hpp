/**
 * @file faio/detail/blocking/service.hpp
 * @brief 有界阻塞执行服务，将同步调用交给独立线程执行。
 */
#pragma once

#include "faio/detail/common/error.hpp"
#include "faio/detail/common/move_only_function.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace faio::blocking {
/**  @brief 原生后端的辅助服务仅在实际使用回退路径时启动应用线程。 */
enum class startup_policy { preheated, on_demand };

/**
 *  @brief 不依赖运行时类型的有界阻塞执行服务。
 * @details readiness 服务在构造时预热；原生后端中不使用操作码的辅助服务在首次提交任务时启动。
 * queue_limit 限制等待执行的任务数，运行中的任务最多 limit 个。
 * 固定池启动全部 limit 个线程，弹性池按需启动并回收空闲线程。
 * 服务负责完成已接纳的任务；close 会排空任务并等待线程退出，不能在线程池内部调用。
 */
class service {
  /** @brief 服务持有的稳定唤醒节点，队列锁保护空闲链表。 */
  struct worker_node {
    /** @brief 唤醒代际；一次等待登记最多由提交者与关闭路径各推进一次。 */
    std::atomic<std::uint32_t> generation{};
    worker_node* next{};
  };

  struct elastic_state {
    explicit elastic_state(std::chrono::milliseconds timeout) : keep_alive(timeout) {}

    const std::chrono::milliseconds keep_alive;
    std::condition_variable ready;
    std::size_t active{};
    std::size_t idle{};
    /** @brief 每个退出的工作线程在锁外等待前一个线程结束，close 再等待最后一个线程结束。 */
    std::jthread last_exiting;
  };

 public:
  /**
   *  @brief 创建有界服务，按启动策略预热线程，或在实际提交任务时启动线程。
   * @param limit 执行线程数上限，运行中的任务最多使用这些服务线程。
   * @param queue_limit 等待队列容量，不包括已经被线程取出的任务。
   * @param min_threads 有效最低线程要求，必须位于 1 到 limit 之间。
   * @param startup readiness 服务预热线程，native 中不使用操作码的服务按需启动。
   */
  explicit service(std::size_t limit = 4,
                   std::size_t queue_limit = 4096,
                   std::size_t min_threads = 1,
                   startup_policy startup = startup_policy::preheated)
      : queue_limit_(queue_limit), thread_limit_(limit) {
    if (!limit || !queue_limit || !min_threads || min_threads > limit)
      throw std::invalid_argument("阻塞执行器线程数/队列容量无效");
    workers_ = std::make_unique<worker_node[]>(limit);
    threads_.reserve(limit);
    try {
      if (startup == startup_policy::preheated)
        start_threads();
    } catch (...) {
      close();
      throw;
    }
  }

  /** @brief 从零线程启动的弹性服务。空闲线程经过 keep_alive 后退出。 */
  explicit service(std::size_t limit, std::size_t queue_limit, std::chrono::milliseconds keep_alive)
      : queue_limit_(queue_limit), thread_limit_(limit) {
    if (!limit || !queue_limit || keep_alive.count() <= 0)
      throw std::invalid_argument("阻塞执行器线程数/队列容量/保活时长无效");
    elastic_ = std::make_unique<elastic_state>(keep_alive);
    threads_.resize(limit);
  }

  /** @brief 禁止复制构造阻塞服务。 */
  service(const service&) = delete;

  /** @brief 禁止复制赋值。 */
  service& operator=(const service&) = delete;

  /** @brief 销毁阻塞服务。 */
  ~service() { close(); }

  /**
   *  @brief 尝试接纳任务，满载时返回 EAGAIN；调用方不得阻塞工作线程等待。
   * @details 小型 noexcept 捕获和类型擦除后的任务只在接纳时转移。其他捕获会在检查容量前构造拥有型包装，以免
   * 在锁内分配；提交失败时，参数可能已被移动。
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
        job_type owned{std::forward<F>(job)};
        return enqueue(std::move(owned));
      }
    } catch (const std::bad_alloc&) {
      return std::unexpected{make_error(ENOMEM)};
    } catch (...) {
      return std::unexpected{make_error(EIO)};
    }
  }

  /**  @brief 停止接受任务，排空已接受任务，然后回收全部阻塞线程。 */
  void close() noexcept {
    std::vector<std::jthread> threads;
    std::jthread last_exiting;
    {
      std::lock_guard lock(mutex_);
      if (closed_)
        return;
      closed_ = true;
      if (elastic_) {
        last_exiting = std::move(elastic_->last_exiting);
      } else {
        for (std::size_t index = 0; index < thread_limit_; ++index)
          workers_[index].generation.fetch_add(1, std::memory_order_release);
      }
      threads.swap(threads_);
    }
    if (elastic_) {
      elastic_->ready.notify_all();
    } else {
      for (std::size_t index = 0; index < thread_limit_; ++index)
        workers_[index].generation.notify_all();
    }
    threads.clear();
  }

  /** @brief 在队列锁内读取尚未领取的任务数，不把运行中的任务算作等待容量。 */
  [[nodiscard]] std::size_t queued() const noexcept {
    std::lock_guard lock(mutex_);
    return jobs_.size();
  }

  /** @brief 返回构造时固定的等待队列容量，不包括正在执行的任务。 */
  [[nodiscard]] std::size_t queue_limit() const noexcept { return queue_limit_; }

  /** @brief 已启动且可处理任务的线程数，包含空闲等待的线程。 */
  [[nodiscard]] std::size_t started_threads() const noexcept {
    std::lock_guard lock(mutex_);
    return elastic_ ? elastic_->active : threads_.size();
  }

 private:
  /** @brief 将任务加入就绪队列。 */
  template <class F>
  expected<void> enqueue(F&& job) {
    std::unique_lock lock(mutex_);
    if (closed_)
      return std::unexpected{make_error(ECANCELED)};
    if (jobs_.size() >= queue_limit_)
      return std::unexpected{make_error(EAGAIN)};
    if (elastic_) {
      /** @brief 队列中的任务已占用了相应数量的空闲线程。 */
      const bool wake = jobs_.size() < elastic_->idle;
      if (!wake && elastic_->active < thread_limit_)
        start_elastic_thread();
      jobs_.emplace_back(std::forward<F>(job));
      lock.unlock();
      if (wake)
        elastic_->ready.notify_one();
      return {};
    }

    start_threads();
    jobs_.emplace_back(std::forward<F>(job));
    auto* wake = idle_;
    if (wake) {
      idle_ = wake->next;
      wake->next = nullptr;
      wake->generation.fetch_add(1, std::memory_order_release);
    }
    lock.unlock();
    if (wake)
      wake->generation.notify_one();
    return {};
  }

  /**
   *  @brief 在构造期间或持有队列锁时启动固定线程，启动失败前不会接纳新任务。
   * @details
   * 已创建的部分线程仍由本服务持有；下次提交时继续补齐，close 会等待它们退出。
   */
  void start_threads() {
    while (threads_.size() < thread_limit_) {
      auto* worker = &workers_[threads_.size()];
      threads_.emplace_back([this, worker] { run(*worker); });
    }
  }

  /** @brief 在队列锁内启动一个线程，复用已经退休的线程槽位。 */
  void start_elastic_thread() {
    const auto slot =
        std::ranges::find_if(threads_, [](const auto& thread) { return !thread.joinable(); });
    assert(slot != threads_.end());
    const auto index = static_cast<std::size_t>(slot - threads_.begin());
    try {
      *slot = std::jthread([this, index] { run_elastic(index); });
      ++elastic_->active;
    } catch (const std::system_error&) {
      /** @brief 操作系统无法增加线程时，已有线程仍可排空队列。 */
      if (elastic_->active == 0)
        throw;
    }
  }

  /** @brief 队列为空时等待任务、关闭或空闲超时，调用时持有队列锁。 */
  bool wait_elastic(std::unique_lock<std::mutex>& lock) {
    using std::chrono::milliseconds;
    auto remaining = elastic_->keep_alive;
    constexpr milliseconds max_interval = std::chrono::hours(24);
    do {
      /** @brief 限制单次等待时长，避免较大的配置时长在换算为截止时间时溢出。 */
      const auto interval = std::min(remaining, max_interval);
      if (elastic_->ready.wait_for(lock, interval, [this] { return closed_ || !jobs_.empty(); }))
        return true;
      remaining -= interval;
    } while (remaining > milliseconds::zero());
    return false;
  }

  void run_elastic(std::size_t index) noexcept {
    std::jthread predecessor;
    for (;;) {
      ::faio::move_only_function<void()> job;
      {
        std::unique_lock lock(mutex_);
        if (jobs_.empty() && !closed_) {
          ++elastic_->idle;
          const bool ready = wait_elastic(lock);
          --elastic_->idle;
          if (!ready)
            predecessor = std::exchange(elastic_->last_exiting, std::move(threads_[index]));
        }
        if (jobs_.empty()) {
          --elastic_->active;
          return;
        }
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }

  /**  @brief 独立服务线程按 FIFO 领取任务；关闭时等待任务排空，不强制中断系统调用。 */
  void run(worker_node& worker) noexcept {
    for (;;) {
      ::faio::move_only_function<void()> job;
      {
        std::unique_lock lock(mutex_);
        while (jobs_.empty() && !closed_) {
          const auto observed = worker.generation.load(std::memory_order_relaxed);
          worker.next = idle_;
          idle_ = &worker;
          lock.unlock();
          worker.generation.wait(observed, std::memory_order_acquire);
          lock.lock();
        }
        if (jobs_.empty())
          return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      try {
        job();
      } catch (...) {
        std::terminate();
      }
    }
  }

  const std::size_t queue_limit_;  /**< 构造后固定的等待容量，try_submit 满载明确返回 EAGAIN。 */
  const std::size_t thread_limit_; /**< 固定执行线程上限，按需启动也不超出此配置。 */
  mutable std::mutex mutex_;       /**< 串行保护任务队列、线程所有权与 closed 发布。 */
  std::unique_ptr<worker_node[]>
      workers_;         /**< 构造时分配地址固定的节点，submit 不再分配唤醒状态。 */
  worker_node* idle_{}; /**< 由同一把锁保护的空闲线程 LIFO 链；任务队列仍遵循 FIFO。 */
  std::deque<::faio::move_only_function<void()>>
      jobs_;                               /**< 已接纳、尚未领取的 FIFO 任务，队列持有其所有权。 */
  std::vector<std::jthread> threads_;      /**< 构造时预留容量，close 移出锁外统一 join。 */
  std::unique_ptr<elastic_state> elastic_; /**< 仅为弹性池分配，状态与队列由同一互斥锁保护。 */
  bool closed_{}; /**< 在同一把锁内标记停止接纳任务；已接纳的任务仍须排空。 */
};

/** @brief 显式对象参数保留具体 Service，提交调用在编译期解析。 */
template <class Derived>
class service_api {
 public:
  /** @brief 尝试提交工作并报告容量不足。 */
  template <class F>
    requires std::invocable<std::decay_t<F>&> && std::constructible_from<std::decay_t<F>, F&&>
  expected<void> try_submit(this const Derived& self, F&& job) noexcept {
    auto* service = self.native_handle();
    if (!service)
      return std::unexpected{make_error(ECANCELED)};
    return service->try_submit(std::forward<F>(job));
  }

  /** @brief 读取已启动的阻塞线程数量。 */
  std::size_t started_threads(this const Derived& self) noexcept {
    auto* service = self.native_handle();
    return service ? service->started_threads() : 0;
  }
};

/**  @brief 具体服务类型的引用；共享租约和显式外部借用各自遵守相应的生命周期约定。 */
template <class Service>
class basic_service_ref : public service_api<basic_service_ref<Service>> {
 public:
  using service_type = Service;
  /** @brief 构造静态服务引用。 */
  basic_service_ref() = default;

  /** @brief 构造静态服务引用。 */
  explicit basic_service_ref(std::shared_ptr<Service> service) noexcept
      : service_(std::move(service)) {}

  /** @brief 构造静态服务引用。 */
  explicit basic_service_ref(Service& service) noexcept : borrowed_(std::addressof(service)) {}

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return native_handle() != nullptr; }

  /** @brief 借用底层服务对象。 */
  Service* native_handle() const noexcept { return service_ ? service_.get() : borrowed_; }

 private:
  std::shared_ptr<Service> service_; /**< 持有服务租约，保证服务在异步任务执行期间存活。 */
  Service* borrowed_{};              /**< 外部所有者必须存活到全部已接纳任务结束。 */
};

using service_ref = basic_service_ref<service>;
}  // namespace faio::blocking
