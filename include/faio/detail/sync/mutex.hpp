#ifndef FAIO_DETAIL_SYNC__mutexHPP
#define FAIO_DETAIL_SYNC__mutexHPP

#include "faio/detail/runtime/core/poller.hpp"
#include <coroutine>

namespace faio::sync {

class mutex {

  // Awaiter类：实现mutex的等待机制，本身是一个链表节点，用于存储等待获取锁的协程。
  // 基于头插法的链表设计：先进后出的lifo设计。
  // 如果是A，B，C按顺序等待加锁，则链表是C->B->A。
  class Awaiter {
    friend mutex;

  public:
    explicit Awaiter(mutex &mutex) : _mutex{mutex} {}

    // 总是挂起
    auto await_ready() const noexcept -> bool { return false; }

    // 当协程被挂起时,尝试获取锁.如果获取锁成功,则不挂起协程,直接返回false;否则挂起协程,返回true.
    auto await_suspend(std::coroutine_handle<> handle) noexcept -> bool {
      // 赋值
      _handle = handle;
      // 获取当前锁状态
      auto state = _mutex._state.load(std::memory_order::relaxed);
      // 死循环
      while (true) {
        // 如果当前锁状态为解锁状态,则使用CAS抢锁.如果抢锁成功,则返回false;否则继续循环.
        // 抢锁成功：返回false，不挂起协程
        // 抢锁失败：继续循环
        if (state == _mutex.unlocking_state()) {
          if (_mutex._state.compare_exchange_weak(state, _mutex.locking_state(),
                                                  std::memory_order::acquire,
                                                  std::memory_order::relaxed)) {
            return false;
          }
        } else {
          // 如果锁状态已经是已锁定状态
          // 头插法：将自己的next指针指向当前链表的头部
          _next = state;
          // 尝试更新当前链表的头部
          if (_mutex._state.compare_exchange_weak(state, this,
                                                  std::memory_order::acquire,
                                                  std::memory_order::relaxed)) {
            break;
          }
        }
      }
      return true;
    }

    constexpr void await_resume() const noexcept {}

  private:
    mutex &_mutex;  // 指向所属的mutex对象
    Awaiter *_next; // 指向下一个等待的协程,形成一个链表
    std::coroutine_handle<>
        _handle; // 当前等待的协程的句柄,用于在解锁时恢复协程
  };

public:
  mutex() : _state{unlocking_state()} {}
  ~mutex() {}
  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;
  mutex(mutex &&) = delete;
  mutex &operator=(mutex &&) = delete;

  // 尝试加锁.如果加锁成功,则返回true;否则返回false。
  // 这不是awaiter，无法使用co_await mutex.try_lock()。
  [[nodiscard]]
  auto try_lock() noexcept -> bool {
    auto state = _state.load(std::memory_order::relaxed);
    return _state.compare_exchange_strong(state, locking_state(),
                                          std::memory_order::acquire,
                                          std::memory_order::relaxed);
  }

  // 加锁
  // 直接返回一个Awaiter对象，用于等待获取锁。
  // 可以使用co_await mutex.lock()来等待获取锁。
  [[nodiscard]]
  auto lock() noexcept {
    return Awaiter{*this};
  }

  // 解锁
  //
  void unlock() noexcept {
    // 如果当前锁状态为解锁状态,则说明没有协程持有锁,这是一个错误的操作.记录错误日志并终止程序.
    if (_state.load(std::memory_order::relaxed) == unlocking_state()) {
      fastlog::console.error("unlocking an unlocked mutex");
      std::terminate();
    }
    // 如果等待队列为空,说明没有协程需要等待获取锁,则直接将锁状态设置为解锁状态.
    if (_fifo_awaiters == nullptr) {
      auto state = _state.load(std::memory_order::relaxed);
      // 如果当前锁状态为已锁定状态,则使用CAS将锁状态设置为解锁状态.
      if (state == locking_state() &&
          _state.compare_exchange_strong(state, unlocking_state(),
                                         std::memory_order::acquire,
                                         std::memory_order::relaxed)) {
        return;
      }
      // 获取到等待队列的头部
      auto lifo_awaiters =
          _state.exchange(locking_state(), std::memory_order::acquire);

      // 利用std::tie反转链表，得到fifo等待链表
      // 循环赋值：
      //  lifo_awaiters赋值给_fifo_awaiters，第一轮：lifo链表头部赋值给fifo链表头部
      // lifo链表的下一个节点赋值给lifo_awaiters
      // 将_fifo_awaiters赋值给lifo_awaiters的next指针。
      do {
        std::tie(_fifo_awaiters, lifo_awaiters, lifo_awaiters->_next) =
            std::tuple{lifo_awaiters, lifo_awaiters->_next, _fifo_awaiters};
      } while (lifo_awaiters != nullptr);
    }
    runtime::detail::push_task_to_local_queue(_fifo_awaiters->_handle);
    _fifo_awaiters = _fifo_awaiters->_next;
  }

private:
  [[nodiscard]]
  // 已锁定状态
  // 表示当前锁状态为已锁定状态，没有等待者。
  auto locking_state() -> Awaiter * {
    return nullptr;
  }

  // 解锁状态
  // 表示当前锁状态为解锁状态，有等待者。
  [[nodiscard]]
  auto unlocking_state() -> Awaiter * {
    return reinterpret_cast<Awaiter *>(this);
  }

private:
  std::atomic<Awaiter *> _state;    // 当前锁状态,同时是Awaiter链表的链头
  Awaiter *_fifo_awaiters{nullptr}; // fifo等待链表头节点，保证先进先出
};

} // namespace faio::sync

#endif // FAIO_DETAIL_SYNC__mutexHPP
