/**
 * @file faio/detail/io/protocol/completion.hpp
 * @brief 具体完成节点与单指针静态借用；结果发布不保存函数指针。
 */
#pragma once
#include "faio/detail/coroutine/scheduler/resume_target.hpp"
#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

namespace faio::io::detail {
/** @brief 持有协程帧的通知协程，初始状态为挂起；发布者领取后恢复或销毁一次。 */
class completion_notification {
 public:
  /** @brief 协程返回值与挂起协议。 */
  struct promise_type {
    /** @brief 创建协程返回对象。 */
    completion_notification get_return_object() noexcept {
      return completion_notification{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    /** @brief 协程创建后保持挂起，等待显式启动。 */
    std::suspend_always initial_suspend() const noexcept { return {}; }

    /** @brief 完成后直接销毁协程帧。 */
    std::suspend_never final_suspend() const noexcept { return {}; }

    /** @brief 标记无返回值协程完成。 */
    void return_void() const noexcept {}

    /** @brief 未捕获异常时终止进程。 */
    void unhandled_exception() const noexcept { std::terminate(); }
  };

  /** @brief 构造完成通知状态。 */
  explicit completion_notification(std::coroutine_handle<> handle) noexcept : handle_(handle) {}

  /** @brief 禁止复制构造完成通知状态。 */
  completion_notification(const completion_notification&) = delete;

  /** @brief 移动构造完成通知状态。 */
  completion_notification(completion_notification&& other) noexcept
      : handle_(std::exchange(other.handle_, {})) {}

  /** @brief 销毁完成通知状态。 */
  ~completion_notification() {
    if (handle_)
      handle_.destroy();
  }

  /** @brief 转出完成通知协程句柄。 */
  std::coroutine_handle<> release() noexcept { return std::exchange(handle_, {}); }

 private:
  std::coroutine_handle<> handle_;
};

/** @brief 域只借用此具体节点；完成后可以立即销毁整个消费者。 */
struct completion_state {
  /** @brief 原生完成事件的关联数据。 */
  struct user_data {
    std::int64_t result{};
    std::uint64_t transferred{};
  } _user_data{};

  std::atomic<unsigned char> gate_{0}; /**< 挂起登记、已挂起或已完成的状态。 */

  /** @brief 构造 I/O 完成状态。 */
  completion_state() = default;
  /** @brief 禁止复制构造 I/O 完成状态。 */
  completion_state(const completion_state&) = delete;
  /** @brief 禁止复制赋值。 */
  completion_state& operator=(const completion_state&) = delete;

  /** @brief 销毁 I/O 完成状态。 */
  ~completion_state() {
    if (notification_)
      notification_.destroy();
  }

  /** @brief 建立操作挂起登记。 */
  template <class S>
  void arm(::faio::basic_scheduler_ref<S> scheduler, std::coroutine_handle<> continuation) {
    arm(::faio::detail::capture_resume_target(scheduler), continuation);
  }

  /** @brief 建立操作挂起登记。 */
  template <class S>
  void arm(::faio::detail::basic_resume_target<S> scheduler, std::coroutine_handle<> continuation) {
    if (notification_ || continuation_ || gate_.load(std::memory_order_relaxed) != 0)
      throw std::logic_error("completion node already armed");
    if constexpr (std::same_as<S, ::faio::default_scheduler>) {
      scheduler_ = scheduler;
      continuation_ = continuation;
    } else {
      auto notification = notify_scheduler(scheduler, continuation);
      notification_ = notification.release();
    }
  }

  /** @brief 先复制全部恢复数据再发布 gate；此后不再读取节点。 */
  void publish(std::int64_t result, std::uint64_t transferred) noexcept {
    const auto scheduler = scheduler_;
    const auto continuation = continuation_;
    const auto notification = std::exchange(notification_, {});
    _user_data = {result, transferred};
    const auto previous = gate_.exchange(2, std::memory_order_acq_rel);
    if (notification) {
      if (previous == 1)
        notification.resume();
      else
        notification.destroy();
    } else if (previous == 1) {
      try {
        scheduler.schedule_io(continuation);
      } catch (...) {
        std::terminate();
      }
    }
  }

 protected:
  /** @brief 仅在域明确拒绝接纳后撤回登记，允许命名 awaiter 保留参数重试。 */
  void discard_unaccepted() noexcept {
    if (auto notification = std::exchange(notification_, {}))
      notification.destroy();
    scheduler_ = {};
    continuation_ = {};
    gate_.store(0, std::memory_order_relaxed);
  }

  /** @brief 领取完成结果。 */
  void receive(completion_notification notification) {
    if (notification_ || gate_.load(std::memory_order_relaxed) == 1)
      throw std::logic_error("completion receiver already pending");
    notification_ = notification.release();
    gate_.store(1, std::memory_order_relaxed);
  }

 private:
  /** @brief 通知调度器存在可执行工作。 */
  template <class S>
  static completion_notification notify_scheduler(::faio::detail::basic_resume_target<S> scheduler,
                                                  std::coroutine_handle<> continuation) {
    scheduler.schedule_io(continuation);
    co_return;
  }

  ::faio::detail::resume_target scheduler_;
  std::coroutine_handle<> continuation_{};
  std::coroutine_handle<> notification_{};
};

/** @brief I/O 完成通知接口。 */
template <class Derived>
class completion_api {
 public:
  /** @brief 发布已准备的结果或任务。 */
  void publish(this const Derived& self, std::int64_t result, std::uint64_t transferred) noexcept {
    if (auto* node = self.native_handle())
      node->publish(result, transferred);
  }
};

/** @brief 指向单个具体节点的指针；没有 consumer 擦除或 publish 操作表。 */
template <class Node>
class basic_completion_ref : public completion_api<basic_completion_ref<Node>> {
 public:
  /** @brief 构造静态完成服务引用。 */
  basic_completion_ref() = default;

  /** @brief 构造静态完成服务引用。 */
  explicit basic_completion_ref(Node& node) noexcept : node_(&node) {}

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return node_ != nullptr; }

  /** @brief 借用具体完成节点。 */
  Node* native_handle() const noexcept { return node_; }

 private:
  Node* node_{};
};

using completion_target = basic_completion_ref<completion_state>;

/**
 * @brief 普通消费者的类型化适配；Consumer::complete 在通知协程中静态调用。
 * 每个 pending operation 独占一个节点；target 重复登记在发布前抛出 logic_error。
 * Consumer 和本节点覆盖接纳后的唯一发布；complete 可以销毁两者。
 */
template <class Consumer>
class completion_receiver : public completion_state {
 public:
  /** @brief 构造具体消费者的完成接收器。 */
  explicit completion_receiver(Consumer& consumer) noexcept : consumer_(&consumer) {}

  /** @brief 取得协程恢复目标。 */
  completion_target target() {
    receive(notify(consumer_, this));
    return completion_target{*this};
  }

 private:
  /** @brief 向等待消费者交付完成结果。 */
  static completion_notification notify(Consumer* consumer, completion_state* node) {
    const auto result = node->_user_data;
    consumer->complete(result.result, result.transferred);
    co_return;
  }

  Consumer* consumer_;
};
}  // namespace faio::io::detail
