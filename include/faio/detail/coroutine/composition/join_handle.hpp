/**
 * @file faio/detail/coroutine/composition/join_handle.hpp
 * @brief 只能移动的任务结果句柄，独占结果消费权。
 */
#pragma once

#include "faio/detail/coroutine/composition/join_state.hpp"
#include "faio/detail/coroutine/context/execution_binding.hpp"
#include <atomic>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace faio {
/**
 * @brief 任务结果句柄，只能移动并独占结果消费权。丢弃句柄不会停止任务；未观察的异常会终止程序。
 * 普通线程可以调用 wait/get，工作线程必须使用 co_await。
 * 用户通过句柄独占结果消费权；共享状态的引用计数只管理状态的生命周期。
 * 不能并发操作同一个句柄对象，普通线程等待与协程等待均只能领取一次结果。
 */
template <class T, class S = default_scheduler>
class [[nodiscard]] join_handle {
 public:
  using value_type = T;
  using scheduler_type = S;

  /** @brief 接管启动入口返回的状态引用，构造本身不再启动任务。 */
  explicit join_handle(std::shared_ptr<detail::join_handle_state<T, S>> state)
      : state_(std::move(state)) {}

  /** @brief 将消费权移交给新句柄，原句柄变为空。 */
  join_handle(join_handle&&) noexcept = default;

  /** @brief 禁止复制消费权，避免多个句柄领取同一个结果。 */
  join_handle(const join_handle&) = delete;

  /** @brief 禁止复制赋值。 */
  join_handle& operator=(const join_handle&) = delete;

  /** @brief 先放弃当前消费权，再接管另一句柄；原任务不会因赋值被取消。 */
  join_handle& operator=(join_handle&& other) noexcept {
    if (this != &other) {
      if (state_)
        state_->abandon();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  /** @brief 析构时放弃尚未领取的结果，不等待或停止运行中的任务。 */
  ~join_handle() {
    if (state_)
      state_->abandon();
  }

  /** @brief 立即查询完成状态；空句柄返回 false，不消费结果。 */
  bool done() const noexcept { return state_ && state_->ready.load(std::memory_order_acquire); }

  /** @brief 对非空句柄发出协作停止请求；不保证任务此时已经结束。 */
  void request_stop() noexcept {
    if (state_)
      state_->stop_source.request_stop();
  }

  /** @brief 返回任务停止令牌，空句柄得到不可停止的空令牌。 */
  std::stop_token stop_token() const noexcept {
    return state_ ? state_->stop_source.get_token() : std::stop_token{};
  }

  /** @brief 普通线程阻塞到任务完成，不领取结果；在工作线程调用会抛出逻辑错误。 */
  void wait() const {
    if (!state_)
      throw std::logic_error("空 JoinHandle");
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 上阻塞等待 JoinHandle");
    if (state_->ready.load(std::memory_order_acquire))
      return;
    state_->blocking_waiter.store(true, std::memory_order_seq_cst);
    for (bool ready = state_->ready.load(std::memory_order_seq_cst); !ready;
         ready = state_->ready.load(std::memory_order_acquire))
      state_->ready.wait(false, std::memory_order_acquire);
  }

  /** @brief 普通线程等待完成并领取一次结果，异常在这里重抛。 */
  T get() {
    wait();
    return state_->take();
  }

  /** @brief JoinHandle 的协程等待者，接管句柄消费权并借用完成事件。 */
  struct awaiter {
    std::shared_ptr<detail::join_handle_state<T, S>>
        state; /**< 维持状态的生命周期并持有唯一消费权，原 JoinHandle 变为空。 */
    typename ::faio::detail::basic_completion_event<S>::awaiter
        completion; /**< 事件等待者，节点直接保存在当前 awaiter 或父协程帧中。 */

    /** @brief 先接管状态，再从该状态创建 completion awaiter；成员顺序不能颠倒。 */
    explicit awaiter(std::shared_ptr<detail::join_handle_state<T, S>> s)
        : state(std::move(s)), completion(state->completion.wait()) {}

    /** @brief 仅在登记前移动 awaiter，转移消费权和未登记的事件节点。 */
    awaiter(awaiter&&) = default;

    /** @brief 禁止复制等待者及消费权。 */
    awaiter(const awaiter&) = delete;

    /** @brief 析构时，尚未消费的结果按句柄放弃规则处理；已消费时 abandon 不执行任何操作。 */
    ~awaiter() {
      if (state)
        state->abandon();
    }

    /** @brief 挂起前检查：任务已完成则立即继续取结果。 */
    bool await_ready() const noexcept { return completion.await_ready(); }

    /** @brief 挂起时：登记完成事件，由事件处理登记与通知竞态。 */
    template <class Promise>
      requires detail::matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      return completion.await_suspend(h);
    }

    /** @brief 恢复时：领取一次结果或重抛任务异常。 */
    T await_resume() { return state->take(); }
  };

  /** @brief 右值等待时把状态从句柄转移到 awaiter，原句柄不再持有消费权。 */
  awaiter operator co_await() && {
    if (!state_)
      throw std::logic_error("空 JoinHandle");
    return awaiter{std::exchange(state_, {})};
  }

  /**
   * @brief 等待左值也会转移结果消费权并清空原句柄，避免二次读取。
   * 等待后不能再通过原句柄调用 get 或 request_stop。
   */
  awaiter operator co_await() & {
    if (!state_)
      throw std::logic_error("空 JoinHandle");
    return awaiter{std::exchange(state_, {})};
  }

 private:
  std::shared_ptr<detail::join_handle_state<T, S>>
      state_; /**< 任务共享状态的引用；句柄独占结果消费权，但状态还由根帧持有。 */
};
}  // namespace faio
