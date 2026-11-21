/**
 * @file faio/detail/runtime/common/completion_service.hpp
 * @brief 原生运行时完成发布、工作线程唤醒与排空协调。
 */
#pragma once

#include "faio/detail/runtime/common/worker_waker_ref.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/domain_scheduler.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace faio::detail {
/** @brief 运行时直接拥有；派生适配宿主共享同一锁和发布者责任，借用不转移所有权。 */
class runtime_completion_service {
 public:
  using waker_ref = ::faio::runtime::detail::basic_target_waker_ref<default_scheduler>;

  /** @brief 构造运行时根任务发布与排空服务。 */
  explicit runtime_completion_service(std::size_t workers = 1) : workers_(workers) {
    if (!workers)
      throw std::invalid_argument("runtime completion service requires a worker");
  }

  /** @brief 禁止复制构造运行时根任务发布与排空服务。 */
  runtime_completion_service(const runtime_completion_service&) = delete;
  /** @brief 禁止复制赋值。 */
  runtime_completion_service& operator=(const runtime_completion_service&) = delete;

  /** @brief 销毁运行时根任务发布与排空服务。 */
  ~runtime_completion_service() {
    if (!quiescent())
      std::terminate();
  }

  /** @brief 设置工作线程唤醒接口。 */
  void set_waker(waker_ref waker) noexcept {
    std::lock_guard lock(mutex_);
    waker_ = waker;
  }

  /** @brief 读取工作线程数量。 */
  std::size_t worker_count() const noexcept { return workers_; }

  /** @brief 任务计数归零后仍须等待完成的发布步骤，不包含 execution 根任务或节点队列。 */
  bool quiescent() const noexcept {
    std::lock_guard lock(mutex_);
    return !publishers_;
  }

  /** @brief 发布运行时停止状态。 */
  void mark_stopped() noexcept {
    std::lock_guard lock(mutex_);
    if (publishers_)
      std::terminate();
    waker_ = {};
    signal_locked();
  }

  /**
   * @brief 在最后一个运行时任务归还计数前取得守卫，保护从计数归零到完成唤醒结束的过程。
   * 不依赖外部根任务；wake 凭证使当前线程驱动器在入睡前等待此处的发布结束。
   */
  class completion_publisher_guard {
   public:
    /** @brief 构造完成发布者保护。 */
    completion_publisher_guard() noexcept = default;

    /** @brief 构造完成发布者保护。 */
    explicit completion_publisher_guard(runtime_completion_service& host) noexcept : host_(&host) {
      std::lock_guard lock(host_->mutex_);
      ++host_->publishers_;
      ++host_->wake_publishers_;
    }

    /** @brief 禁止复制构造完成发布者保护。 */
    completion_publisher_guard(const completion_publisher_guard&) = delete;
    /** @brief 禁止复制赋值。 */
    completion_publisher_guard& operator=(const completion_publisher_guard&) = delete;

    /** @brief 移动构造完成发布者保护。 */
    completion_publisher_guard(completion_publisher_guard&& other) noexcept
        : host_(std::exchange(other.host_, nullptr)) {}

    /** @brief 销毁完成发布者保护。 */
    ~completion_publisher_guard() {
      if (host_) {
        std::lock_guard lock(host_->mutex_);
        --host_->publishers_;
        --host_->wake_publishers_;
        host_->signal_locked();
      }
    }

   private:
    runtime_completion_service* host_{};
  };

  /** @brief 发布任务完成通知。 */
  void notify_completion() noexcept {
    notify_completion_with([this] { return waker_; });
  }

  /** @brief 发布任务完成通知。 */
  template <class W>
  void notify_completion(::faio::runtime::detail::basic_target_waker_ref<W> waker) noexcept {
    notify_completion_with([waker] { return waker; });
  }

 private:
  /** @brief 发布任务完成通知，并通过选定的唤醒端通知各工作线程。 */
  template <class SelectWaker>
  void notify_completion_with(SelectWaker select_waker) noexcept {
    decltype(select_waker()) waker;
    {
      std::lock_guard lock(mutex_);
      signal_locked();
      waker = select_waker();
      ++publishers_;
      ++wake_publishers_;
    }
    for (std::size_t i = 0; i < workers_; ++i)
      waker.wake_worker(i);
    {
      std::lock_guard lock(mutex_);
      --publishers_;
      --wake_publishers_;
      signal_locked();
    }
  }

 public:
  /** @brief 内核唤醒可能在发布者释放最后一个静默凭证前被消费；单线程驱动器在最终完成检查和内核等待前，须等待这段有界的收尾发布结束。用户接收器守卫不参与此等待，避免任意用户代码阻塞 I/O。 */
  bool wait_for_wake_publisher() noexcept {
    std::size_t observed;
    {
      std::lock_guard lock(mutex_);
      if (!wake_publishers_)
        return false;
      observed = revision_.load(std::memory_order_acquire);
    }
    revision_.wait(observed, std::memory_order_acquire);
    return true;
  }

  /** @brief 等待条件满足或到达截止时间。 */
  template <class Predicate>
  void wait_until(Predicate done) {
    while (!done()) {
      const auto observed = revision_.load(std::memory_order_acquire);
      if (done())
        return;
      revision_.wait(observed, std::memory_order_acquire);
    }
  }

 protected:
  /** @brief 在状态锁内发布控制唤醒信号。 */
  void signal_locked() noexcept {
    revision_.fetch_add(1, std::memory_order_release);
    revision_.notify_all();
  }

  mutable std::mutex mutex_;             /**< 保护完成发布状态；实际唤醒在锁外执行。 */
  std::atomic<std::size_t> revision_{0}; /**< 状态变化的修订计数，配合原子等待避免丢失通知。 */
  std::size_t publishers_{};             /**< 仍在锁外发布结果的线程数。 */
  std::size_t wake_publishers_{};        /**< 仍可能发送内核通知的发布者数量。 */
  waker_ref waker_{};                    /**< 借用具体调度域，发布责任保护其锁外调用。 */
  const std::size_t workers_;
};
}  // namespace faio::detail
