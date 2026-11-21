/**
 * @file faio/detail/coroutine/scheduler/serial_domain.hpp
 * @brief 跨挂起独占的协程串行域。
 */
#pragma once
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/coroutine/wait/completion_event.hpp"
#include "faio/detail/sync/mutex.hpp"
#include <functional>
#include <memory>
#include <mutex>

namespace faio::experimental {
/** @brief 串行化整个用户任务。域内任务不得等待自己的域或排在自身之后的工作。 */
template <class S = default_scheduler>
class serial_domain {
  struct control {
    sync::basic_mutex<S> body;
    std::mutex admission;
    ::faio::detail::basic_completion_event<S> drained;
    std::size_t pending{};
    bool closed{};

    /** @brief 关闭接纳后，最后一个调用归还计数时发布排空状态。 */
    void finish() {
      const auto wake = [&] {
        std::lock_guard lock(admission);
        return --pending == 0 && closed;
      }();
      if (wake)
        drained.notify();
    }
  };

  struct ticket {
    control& owner;

    ~ticket() { owner.finish(); }
  };

 public:
  serial_domain() = default;
  serial_domain(const serial_domain&) = delete;
  serial_domain& operator=(const serial_domain&) = delete;

  /** @brief 同步取得域状态的共享所有权，使惰性任务可以安全地晚于外层对象销毁。 */
  template <class Factory>
  auto run(Factory factory) {
    return run_owned(owner_, std::move(factory));
  }

  /** @brief 关闭接纳，并等待所有已接纳的调用退出。 */
  task<void, S> close() { return close_owned(owner_); }

 private:
  template <class Factory>
  static auto run_owned(std::shared_ptr<control> owner, Factory factory)
      -> std::invoke_result_t<Factory&> {
    using R = typename std::invoke_result_t<Factory&>::value_type;
    {
      std::lock_guard lock(owner->admission);
      if (owner->closed)
        throw std::logic_error("串行域已关闭");
      ++owner->pending;
    }
    const ticket active{*owner};
    const auto guard = co_await owner->body.scoped_lock();
    if ((co_await this_coro::stop_token()).stop_requested())
      throw operation_cancelled{};
    if constexpr (std::is_void_v<R>)
      co_await std::invoke(factory);
    else
      co_return co_await std::invoke(factory);
  }

  static task<void, S> close_owned(std::shared_ptr<control> owner) {
    auto target = ::faio::detail::capture_current_resume_target<S>();
    const auto empty = [&] {
      std::lock_guard lock(owner->admission);
      owner->closed = true;
      return owner->pending == 0;
    }();
    if (empty)
      owner->drained.notify();
    co_await owner->drained.wait(std::move(target));
  }

  std::shared_ptr<control> owner_ = std::make_shared<control>();
};
}  // namespace faio::experimental

namespace faio {
using experimental::serial_domain;
}  // namespace faio
