#ifndef FAIO_DETAIL_SYNC_CONDITION_VARIABLE_HPP
#define FAIO_DETAIL_SYNC_CONDITION_VARIABLE_HPP

#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/sync/mutex.hpp"
namespace faio::sync {

class condition_variable {
  class Awaiter {
    friend condition_variable;

  public:
    Awaiter(condition_variable &cv, mutex &mutex) : _cv{cv}, _mutex{mutex} {}

    auto await_ready() const noexcept -> bool { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
      // 赋值
      _handle = handle;
      // 持锁
      std::lock_guard<mutex> lock{_mutex, std::adopt_lock};
      // 头插法：将自己的next指针指向当前链表的头部
      _next = _cv._awaiters.load(std::memory_order::relaxed);
      // 尝试更新当前链表的头部
      while (!_cv._awaiters.compare_exchange_weak(_next, this,
                                                  std::memory_order::acquire,
                                                  std::memory_order::relaxed)) {
      }
    }

    void await_resume() const noexcept {}

  private:
    condition_variable &_cv;
    mutex &_mutex;
    std::coroutine_handle<> _handle;
    Awaiter *_next{nullptr};
  };

public:
  condition_variable() = default;

  // Delete copy
  condition_variable(const condition_variable &) = delete;
  auto operator=(const condition_variable &) -> condition_variable & = delete;
  // Delete move
  condition_variable(condition_variable &&) = delete;
  auto operator=(condition_variable &&) -> condition_variable & = delete;

  // 唤醒一个等待的协程
  void notify_one() noexcept {
    // 获取等待链表的头部
    auto awaiters = _awaiters.load(std::memory_order::relaxed);
    // 如果等待链表为空，则返回
    if (awaiters == nullptr) {
      return;
    }

    // 更新等待链表的头部，将下一个节点设置为等待链表的头部
    while (!_awaiters.compare_exchange_weak(awaiters, awaiters->_next,
                                            std::memory_order::acq_rel,
                                            std::memory_order::relaxed)) {
    }
    // 将下一个节点设置为空
    awaiters->_next = nullptr;
    // 唤醒协程
    condition_variable::resume(awaiters);
  }

  // 唤醒所有等待的协程
  void notify_all() noexcept {
    // 获取等待链表的头部
    auto awaiters = _awaiters.load(std::memory_order::relaxed);
    // 更新等待链表的头部，将等待链表设置为空
    while (!_awaiters.compare_exchange_weak(awaiters, nullptr,
                                            std::memory_order::acq_rel,
                                            std::memory_order::relaxed)) {
    }
    // 唤醒协程
    condition_variable::resume(awaiters);
  }

  // 等待条件变量
  template <class Predicate>
    requires std::is_invocable_r_v<bool, Predicate>
  auto wait(mutex &mutex, Predicate &&predicate) -> task<void> {
    while (!predicate()) {
      co_await Awaiter{*this, mutex};
      co_await mutex.lock();
    }
    co_return;
  }

private:
  // 唤醒一个等待的协程
  static void resume(Awaiter *awaiter) {
    // 循环唤醒等待的协程
    while (awaiter != nullptr) {
      // 唤醒协程
      runtime::detail::push_task_to_local_queue(awaiter->_handle);
      // 移动到下一个等待的协程
      awaiter = awaiter->_next;
    }
  }

private:
  std::atomic<Awaiter *> _awaiters{nullptr}; // 等待链表的头部
};

} // namespace faio::sync

#endif // FAIO_DETAIL_SYNC_CONDITION_VARIABLE_HPP
