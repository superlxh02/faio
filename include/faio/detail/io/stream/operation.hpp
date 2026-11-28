/**
 * @file faio/detail/io/stream/operation.hpp
 * @brief TLS 与进程操作共用的停止令牌绑定与首次终态仲裁。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/composition/select.hpp"
#include "faio/detail/coroutine/context/this_coro.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/stream/operation_options.hpp"
#include "faio/detail/time/time.hpp"
#include <cerrno>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace faio::io::detail {
/**
 * @brief 保留子任务继承的上下文，仅替换它的停止令牌，不修改调用者上下文。
 * @tparam T 被包装任务的结果类型。
 */
template <class T, class S = default_scheduler>
class stop_bound_task {
 public:
  using scheduler_type = S;

  /**
   * @brief 接管一个惰性任务及其专用停止令牌。
   * @param child 尚未等待的子任务。
   * @param token 子任务执行期间使用的停止令牌。
   */
  stop_bound_task(task<T, S> child, std::stop_token token)
      : child_(std::move(child)), token_(std::move(token)) {}

  /** @brief 在普通任务挂起握手完成后替换子任务的停止上下文。 */
  struct awaiter {
    typename task<T, S>::awaiter inner; /**< 原始任务等待器，负责帧所有权与结果传递。 */
    std::stop_token token;              /**< 即将安装到子任务上下文的停止令牌。 */

    /** @brief 复用原始任务的就绪判断。 */
    bool await_ready() const noexcept { return inner.await_ready(); }

    /**
     * @brief 先继承父任务完整上下文，再为子任务安装专用停止令牌。
     * @tparam Parent 父协程的承诺类型。
     * @param parent 等待该子任务的父协程句柄。
     * @return 普通任务等待器选定的下一个恢复句柄。
     */
    template <class Parent>
    auto await_suspend(std::coroutine_handle<Parent> parent) -> std::coroutine_handle<> {
      auto handle = inner.await_suspend(parent);
      inner.callee.promise().context.stop_token = token;
      ::faio::detail::restore_task_context(inner.callee.promise().context);
      return handle;
    }

    /** @brief 原样取回子任务结果，或传播其异常。 */
    decltype(auto) await_resume() { return inner.await_resume(); }
  };

  /**
   * @brief 仅允许消费右值包装器，向等待器转移任务及令牌。
   * @return 拥有原始任务等待器的适配对象。
   */
  auto operator co_await() && -> awaiter {
    return {std::move(child_).operator co_await(), std::move(token_)};
  }

 private:
  task<T, S> child_;      /**< 尚未转交等待器的子任务。 */
  std::stop_token token_; /**< 子任务专用的停止令牌。 */
};

/**
 * @brief 为一个子任务绑定独立停止令牌。
 * @tparam T 子任务结果类型。
 * @param child 待消费的惰性任务。
 * @param token 替换子任务继承令牌的停止令牌。
 * @return 可直接 co_await 的任务包装器。
 */
template <class T, class S>
auto bind_stop(task<T, S> child, std::stop_token token) -> stop_bound_task<T, S> {
  return {std::move(child), std::move(token)};
}

/**
 * @brief 在完成、取消与截止时间之间只提交首次终态，并持久保存该结果。
 * @tparam T 操作成功结果的类型，可以为 void。
 */
template <class T>
class stream_operation_state {
  /** @brief 将外部停止回调转发为当前操作的统一取消请求。 */
  struct forward_stop {
    stream_operation_state* state; /**< 回调借用的操作状态，回调必须先于该状态销毁。 */

    /** @brief 以 ECANCELED 竞争终态；已完成时不覆盖现有结果。 */
    void operator()() const noexcept { state->try_cancel(::faio::error{ECANCELED}); }
  };

 public:
  /**
   * @brief 注册父级和显式停止源，并立即处理已经过期的截止时间。
   * @param options 本次操作的绝对截止时间及附加停止令牌。
   * @param parent 当前调用任务继承的停止令牌。
   */
  stream_operation_state(io::operation_options options, std::stop_token parent)
      : options_(std::move(options)) {
    if (parent.stop_possible())
      parent_.emplace(parent, forward_stop{this});
    if (options_.stop.stop_possible())
      explicit_.emplace(options_.stop, forward_stop{this});
    if (options_.deadline && std::chrono::steady_clock::now() >= *options_.deadline)
      try_cancel(::faio::error{ETIMEDOUT});
  }

  /** @brief 禁止复制此状态，其中包含锁和引用自身的停止回调。 */
  stream_operation_state(const stream_operation_state&) = delete;
  /** @brief 禁止复制赋值，保持回调地址和结果归属稳定。 */
  auto operator=(const stream_operation_state&) -> stream_operation_state& = delete;

  /** @brief 取得向底层任务广播取消的统一停止令牌。 */
  auto token() const noexcept -> std::stop_token { return stop_.get_token(); }

  /**
   * @brief 检查终态；尚未终结而时间已到时，直接提交超时并请求底层停止。
   * @return 是否已经存在完成、取消或超时结果。
   */
  auto done() const noexcept -> bool {
    bool expired = false;
    {
      std::lock_guard lock(mutex_);
      if (terminal_)
        return true;
      if (options_.deadline && std::chrono::steady_clock::now() >= *options_.deadline) {
        result_.emplace(std::unexpected(::faio::error{ETIMEDOUT}));
        terminal_ = true;
        expired = true;
      }
    }
    if (expired)
      stop_.request_stop();
    return expired;
  }

  /**
   * @brief 在未终结且未过期时执行提交函数；成功和错误结果都只接收一次。
   * @tparam Commit 无异常、返回 expected<T> 的提交函数类型。
   * @param commit 在结果锁内执行的短小提交动作，不得再次获取同一把锁。
   * @return 本次提交是否被接收；已有终态或截止时间已到时返回 false。
   */
  template <class Commit>
    requires std::same_as<std::invoke_result_t<Commit&>, expected<T>>
             && std::is_nothrow_invocable_v<Commit&>
  auto try_commit(Commit&& commit) -> bool {
    bool expired = false;
    {
      std::lock_guard lock(mutex_);
      if (terminal_)
        return false;
      if (options_.deadline && std::chrono::steady_clock::now() >= *options_.deadline) {
        result_.emplace(std::unexpected(::faio::error{ETIMEDOUT}));
        terminal_ = true;
        expired = true;
      } else {
        result_.emplace(std::invoke(commit));
        terminal_ = true;
      }
    }
    if (expired)
      stop_.request_stop();
    return !expired;
  }

  /**
   * @brief 尝试以取消或超时错误结束操作，并停止尚未排空的底层工作。
   * @param reason 本次取消独立持有的错误，保留原生原因及已完成进度。
   * @return 是否由本次请求提交了首次终态。
   */
  auto try_cancel(error reason) -> bool {
    {
      std::lock_guard lock(mutex_);
      if (terminal_)
        return false;
      result_.emplace(std::unexpected(std::move(reason)));
      terminal_ = true;
    }
    stop_.request_stop();
    return true;
  }

  /**
   * @brief 在操作与底层清理完成后消费唯一结果。
   * @return 本次操作保存的成功值或错误；重复消费或过早调用抛出逻辑错误。
   */
  auto take_result() -> expected<T> {
    std::lock_guard lock(mutex_);
    if (!terminal_ || !result_)
      throw std::logic_error("operation result not available");
    auto result = std::move(*result_);
    result_.reset();
    return result;
  }

 private:
  io::operation_options options_;             /**< 操作开始时固定的截止时间与附加停止源。 */
  mutable std::mutex mutex_;                  /**< 保护唯一终态与结果槽位。 */
  mutable std::stop_source stop_;             /**< 向实际执行任务广播取消的统一停止源。 */
  mutable bool terminal_{};                   /**< 是否已提交不可覆盖的完成、取消或超时结果。 */
  mutable std::optional<expected<T>> result_; /**< 尚未被调用者消费的唯一结果。 */
  std::optional<std::stop_callback<forward_stop>>
      parent_; /**< 父停止回调；后声明以便先于所借用状态销毁。 */
  std::optional<std::stop_callback<forward_stop>>
      explicit_; /**< 显式停止回调；注销时等待正在运行的回调结束。 */
};

/**
 * @brief 执行操作主体，并把 select 的停止请求连接到操作统一停止源。
 * @tparam T 操作结果类型。
 * @tparam body_type 接收 stream_operation_state<T> 引用并返回相同静态调度器 task<void, S> 的函数对象类型。
 * @param state 保存终态并控制取消的状态。
 * @param body 实际操作主体，成功结束前必须提交结果。
 * @param failure 保存主体抛出的非取消异常，待清理后重新抛出。
 * @return 完成主体及其取消处理的任务。
 */
template <class T, class body_type>
auto operation_body(stream_operation_state<T>& state, body_type& body, std::exception_ptr& failure)
    -> task<void,
            typename std::invoke_result_t<body_type&, stream_operation_state<T>&>::scheduler_type> {
  const auto inherited = co_await this_coro::stop_token();
  std::stop_callback selected_stop{inherited,
                                   [&state] { state.try_cancel(::faio::error{ECANCELED}); }};
  try {
    if (!state.done())
      co_await bind_stop(body(state), state.token());
    if (!state.done())
      throw std::logic_error("operation body did not commit a result");
  } catch (const operation_cancelled&) {
    state.try_cancel(::faio::error{ECANCELED});
  } catch (...) {
    failure = std::current_exception();
    state.try_cancel(::faio::error{ECANCELED});
  }
}

/**
 * @brief 到达绝对截止时间后竞争超时终态。
 * @tparam T 操作结果类型。
 * @param state 与主体共享的终态仲裁状态。
 * @param deadline 使用单调时钟的绝对截止时间。
 * @return 定时器任务，主体先完成时可安全取消并排空。
 */
template <class T>
auto operation_timer(stream_operation_state<T>& state,
                     std::chrono::steady_clock::time_point deadline) -> task<void> {
  try {
    co_await time::sleep_until(deadline);
    state.try_cancel(::faio::error{ETIMEDOUT});
  } catch (const operation_cancelled&) {
  }
}

/**
 * @brief 统一执行带截止时间和取消的操作，并在返回前排空主体及定时器。
 * @tparam T 成功结果类型。
 * @tparam body_type 接收操作状态并显式提交结果的协程函数对象类型。
 * @param options 绝对截止时间与附加停止令牌。
 * @param body 操作主体，按值保存在本协程帧内。
 * @return 首次提交的结果；主体非取消异常在排空后重新抛出。
 */
template <class T, class body_type>
  requires std::same_as<std::invoke_result_t<body_type&, stream_operation_state<T>&>, task<void>>
auto run_operation(io::operation_options options, body_type body) -> task<expected<T>> {
  auto parent = co_await this_coro::stop_token();
  stream_operation_state<T> state(options, parent);
  std::exception_ptr failure;
  if (options.deadline) {
    co_await select(operation_body(state, body, failure),
                    operation_timer(state, *options.deadline));
  } else {
    co_await operation_body(state, body, failure);
  }
  if (failure)
    std::rethrow_exception(failure);
  co_return state.take_result();
}

/**
 * @brief 在调用方截止时间与接口默认超时之间选择较早的一个。
 * @param options 调用者提供的操作选项。
 * @param timeout 从当前单调时钟时刻开始计算的默认最长持续时间。
 * @return 应用了较早截止时间的选项副本，原停止令牌保持不变。
 */
inline auto with_default_deadline(io::operation_options options,
                                  std::chrono::steady_clock::duration timeout)
    -> io::operation_options {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  if (!options.deadline || deadline < *options.deadline)
    options.deadline = deadline;
  return options;
}
}  // namespace faio::io::detail
