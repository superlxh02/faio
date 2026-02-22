#ifndef FAIO_DETAIL_COROUTINE_TASK_HPP
#define FAIO_DETAIL_COROUTINE_TASK_HPP
#include "faio/detail/common/util/noncopyable.hpp"
#include "fastlog/fastlog.hpp"
#include <coroutine>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <type_traits>

/*
-------------------关于caller和callee-------------------
caller: 调用co_await的协程
callee: 被co_await的协程
例子------------------------------------------------------
task<void> task1() {  }
task<void> task2() { co_await task1(); }
task1 是 callee（被调用者）—— 它是被 co_await 等待的那个协程。
task2 是 caller（调用者）—— 它是发起 co_await 的那个协程。

这个例子中，调用的是task1的awaiter
task1的awaiter描述的是task2的挂起逻辑和恢复逻辑
*/

/*
此文件目的：封装协程，并且让协程实现awaiter机制，让co_await可以接受协程

此文件代码描述的协程主要行为：
1.规定我初始默认挂起
  我被创建后不会立即执行，而是挂起等待，直到有人co_await我或者显式resume我。

2.规定我结束时的行为
  我执行完毕后，会挂起自己而不是直接销毁。
  如果有人在等我，我会把执行权交还给它，让它继续运行。
  如果没人在等我（我是顶层协程），我会检查自己是否携带了异常：
    有异常则报告并终止程序；
    无异常则安静结束。

3.规定调用我的协程挂起时的行为
  如果我已经完成了，调用我的协程不需要挂起，直接继续运行。
  如果我还没完成，调用我的协程会挂起自己，记住它在等我，然后把执行权交给我，让我开始运行。

4.规定调用我的协程恢复时的行为
  我完成后，调用我的协程被恢复，它会从我这里取走结果。
  如果我执行期间发生了异常，这个异常会在它取结果时重新抛出。

*/

namespace faio {
template <typename T> class task;

namespace detail {
// 基础任务承诺,不带返回值相关处理

struct base_task_promise {

  // 完成回调：协程结束时调用（用于 spawn 的 tracker 追踪等）
  // 零开销设计：不使用时为 nullptr，无堆分配
  using completion_callback_t = void (*)(void *);
  completion_callback_t _on_complete{nullptr};
  void *_on_complete_arg{nullptr};

  // 最后的awaiter
  struct final_awaiter {
    // 始终挂起
    constexpr bool await_ready() const noexcept { return false; }

    // 设置最后一个协程挂起逻辑
    // 如果最后一个协程有调用者，则返回调用者，否则返回空协程
    template <typename T>
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<T> callee) const noexcept {
      // 触发完成回调（如果有）
      if (callee.promise()._on_complete) {
        callee.promise()._on_complete(callee.promise()._on_complete_arg);
      }

      if (callee.promise()._caller) {
        return callee.promise()._caller;
      } else {
        if (callee.promise()._exception != nullptr) {
          try {
            std::rethrow_exception(callee.promise()._exception);
          } catch (const std::exception &ex) {
            std::cerr << std::format("catch a exception: {}", ex.what());
            std::terminate();
          } catch (...) {
            std::cerr << "catch a unknown exception\n";
            std::terminate();
          }
        }
        return std::noop_coroutine();
      }
    }
    // 恢复协程,没有逻辑
    constexpr void await_resume() const noexcept {}
  };

  // 初始挂起
  constexpr std::suspend_always initial_suspend() const noexcept { return {}; }
  // 最终返回最后的awaiter
  constexpr final_awaiter final_suspend() const noexcept { return {}; }

  // 未处理的异常
  void unhandled_exception() noexcept {
    _exception = std::move(std::current_exception());
  }

  std::coroutine_handle<> _caller{nullptr}; // 调用者协程句柄

  std::exception_ptr _exception{nullptr}; // 异常
};

// 继承base_task_promise,实现返回值相关处理
template <typename T> class task_promise : public base_task_promise {
public:
  task<T> get_return_object() noexcept;

  // 返回值,要求可以转换为T类型，并且可以构造T类型
  template <typename U>
    requires std::is_convertible_v<U &&, T> && std::is_constructible_v<T, U &&>
  void return_value(U &&value) {
    _value = std::forward<U>(value);
  }

  // 获取结果，左值
  auto expected() & -> T & {
    if (_exception != nullptr) {
      std::rethrow_exception(_exception);
    }
    return _value.value();
  }

  // 获取结果，右值
  auto expected() && -> T && {
    if (_exception != nullptr) {
      std::rethrow_exception(_exception);
    }
    return std::move(_value.value());
  }

private:
  std::optional<T> _value{std::nullopt};
};

// void特化版本的task_promsie
template <> struct task_promise<void> final : public base_task_promise {
  task<void> get_return_object() noexcept;

  void return_void() {}

  void expected() {
    if (_exception != nullptr) {
      std::rethrow_exception(_exception);
    }
  }
};

}; // namespace detail

template <typename T = void> class task : public util::Noncopyable {
public:
  using promise_type = detail::task_promise<T>;

public:
  task() noexcept = default;

  explicit task(std::coroutine_handle<promise_type> handle) : _handle(handle) {}

  ~task() {
    if (_handle) {
      _handle.destroy();
    }
  }

  task(task &&other) noexcept : _handle(std::move(other._handle)) {
    other._handle = nullptr;
  }

  task &operator=(task &&other) noexcept {
    if (std::addressof(other) != this) [[likely]] {
      if (_handle) {
        _handle.destroy();
      }
      _handle = std::move(other._handle);
      other._handle = nullptr;
    }
    return *this;
  }

  // 重载co_await操作符，左值
  // 返回自定义awaiter
  auto operator co_await() const & noexcept {
    // 实现awaiter子类，继承base_awaiter
    // 实现await_resume方法，返回协程结果
    struct awaiter : public base_awaiter {
      decltype(auto) await_resume() const noexcept {
        if (!this->_callee) {
          throw std::logic_error("handle is nullptr");
        }
        return this->_callee.promise().expected();
      }
    };
    return awaiter{_handle};
  }
  // 重载co_await操作符，右值
  auto operator co_await() && noexcept {
    struct awaiter : public base_awaiter {
      using base_awaiter::base_awaiter;

      auto await_resume() {
        if (!this->_callee) {
          fastlog::console.error("handle is nullptr");
          std::terminate();
        }

        if constexpr (std::is_same_v<T, void>) {
          this->_callee.promise().expected();
          this->_callee.destroy();
          this->_callee = nullptr;
          return;
        } else {
          T value = std::move(this->_callee.promise().expected());
          this->_callee.destroy();
          this->_callee = nullptr;
          return value;
        }
      }
    };

    auto handle = _handle;
    _handle = nullptr;
    return awaiter{handle};
  }

public:
  // 拿取协程句柄，并移动当前协程句柄为空
  std::coroutine_handle<promise_type> take() {
    if (_handle == nullptr) {
      throw std::logic_error("handle is nullptr");
    }
    auto res = _handle;
    _handle = nullptr; // 释放所有权，防止析构时 destroy 活跃协程
    return res;
  }

  // 获取协程句柄
  std::coroutine_handle<promise_type> handle() { return _handle; }

  // 恢复协程
  void resume() { _handle.resume(); }

private:
  // 一个awaiter基类，用于co_await重载
  // 实现了await_ready,await_suspend
  // 需要子类实现await_resume
  struct base_awaiter {

    std::coroutine_handle<promise_type> _callee; // 协程句柄，当前协程句柄
    base_awaiter(std::coroutine_handle<promise_type> callee) noexcept
        : _callee(callee) {}

    // 如果协程句柄为空或协程已完成，则返回true
    constexpr bool await_ready() const noexcept {
      return !_callee || _callee.done();
    }

    // 协程挂起逻辑
    //  如果此接口调用者自身还有调用者，则设置调用者为协程的调用者,并返回协程句柄
    template <typename promisetype>
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<promisetype> caller) const noexcept {
      _callee.promise()._caller = caller;
      return _callee;
    }
  };

private:
  std::coroutine_handle<promise_type> _handle{nullptr}; // 协程句柄
};

// 由于之前task类是没有实现的，所以get_return_object方法无法实现
// 现在task类实现了，所以可以实现get_return_object方法

// 实现task_promise<T>::get_return_object
template <typename T>
inline auto detail::task_promise<T>::get_return_object() noexcept -> task<T> {
  return task<T>{std::coroutine_handle<task_promise<T>>::from_promise(*this)};
}

// 实现task_promise<void>::get_return_object
inline auto detail::task_promise<void>::get_return_object() noexcept
    -> task<void> {
  return task<void>{
      std::coroutine_handle<task_promise<void>>::from_promise(*this)};
}

} // namespace faio
#endif // FAIO_DETAIL_COROUTINE_TASK_HPP
