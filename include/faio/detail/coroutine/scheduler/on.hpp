/**
 * @file faio/detail/coroutine/scheduler/on.hpp
 * @brief 结构化跨运行时任务入口与关闭握手。
 */
#pragma once

#include "faio/detail/coroutine/awaiter.hpp"
#include "faio/detail/coroutine/composition/with_stop_token.hpp"
#include "faio/detail/runtime/context.hpp"
#include <concepts>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace faio::experimental {

template <class S>
class basic_runtime_anchor;
template <class S, class T>
task<T, S> on(basic_runtime_anchor<S> target, task<T, S> child);

namespace detail {
using ::faio::detail::additional_stop_state;
using ::faio::detail::capture_task_context;
using ::faio::detail::current_task_context;
using ::faio::detail::root_task;
using ::faio::detail::schedule_task;
using context_type = ::faio::runtime::detail::runtime_context;

/** @brief wrapper 独占运行时；anchor 共享此准入锁和只能关闭一次的准入状态。 */
struct runtime_anchor_state {
  std::mutex mutex;
  context_type* active{};
};

/** @brief 在准入锁内登记目标根任务的生命周期责任，确保目标调度器在提交结束前有效。 */
class target_admission {
 public:
  explicit target_admission(runtime_anchor_state& target) {
    const std::lock_guard lock{target.mutex};
    auto* runtime = target.active;
    if (!runtime)
      throw std::logic_error("on target is closed");
    scheduler = runtime->scheduler();
    lifetime = runtime->task_lifetime();
    stop = runtime->stop_token();
    lifetime.register_task();
  }

  target_admission(const target_admission&) = delete;
  target_admission& operator=(const target_admission&) = delete;

  ~target_admission() { lifetime.finish_task(); }

  scheduler_ref scheduler;
  task_lifetime_ref lifetime;
  std::stop_token stop;
};

/** @brief 复用普通 task 的值与异常语义，以及来源 make_awaiter 维持生命周期的协议。 */
template <class T, class S>
struct on_operation {
  using stored_type = std::conditional_t<std::is_void_v<T>, bool, T>;
  using completion_type = ::faio::detail::awaiter_completion<on_operation, S>;

  std::shared_ptr<runtime_anchor_state> target;
  task<T, S> child;
  std::optional<stored_type> result;
  std::exception_ptr exception;

  /** @brief 构造惰性迁移操作，由 make_awaiter 持有共享稳定状态。 */
  on_operation(std::shared_ptr<runtime_anchor_state> destination, task<T, S> work)
      : target(std::move(destination)), child(std::move(work)) {}

  /** @brief 避免编译时要求 T 可移动；结果只在稳定状态已启动后构造。 */
  on_operation(on_operation&& other) noexcept
      : target(std::move(other.target)), child(std::move(other.child)) {}

  /** @brief 跨运行时根协程包装：在目标上下文等待子任务，保存值或异常后发布完成。 */
  static root_task root_task_wrapper(on_operation* operation,
                                     std::shared_ptr<completion_type> completion,
                                     task<T, S> work) {
    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(work);
        operation->result.emplace(true);
      } else {
        operation->result.emplace(co_await std::move(work));
      }
    } catch (...) {
      operation->exception = std::current_exception();
    }
    std::move(*completion).complete();
  }

  /** @brief 检查准入与登记在同一锁内完成；拒绝也恰好完成一次。 */
  void on_suspend(completion_type completion) noexcept {
    std::shared_ptr<completion_type> owned;
    try {
      owned = std::make_shared<completion_type>(std::move(completion));
      const auto parent = capture_task_context();
      const target_admission admitted{*target};
      auto stop = std::make_shared<additional_stop_state>(admitted.stop, parent);
      ::faio::detail::task_context context{};
      context.stop_token = stop->source.get_token();
      context.cancellation_owner = std::move(stop);
      context.cancellation_policy = parent.cancellation_policy;
      auto root = root_task_wrapper(this, owned, std::move(child));
      root.set_context(std::move(context));
      schedule_task(std::move(root), admitted.scheduler, nullptr, admitted.lifetime);
    } catch (...) {
      exception = std::current_exception();
      if (owned)
        std::move(*owned).complete();
      else
        std::move(completion).complete();
    }
  }

  /** @brief 沿用普通结果路径的异常处理和移动消费语义。 */
  T on_resume() && {
    if (exception)
      std::rethrow_exception(exception);
    if constexpr (!std::is_void_v<T>)
      return std::move(result.value());
  }
};
}  // namespace detail

/** @brief 目标准入凭据，关停以后仍可安全检测拒绝。 */
template <class S = default_scheduler>
class basic_runtime_anchor {
  static_assert(std::same_as<S, default_scheduler>);
  friend class anchored_runtime;
  template <class U, class T>
  friend task<T, U> on(basic_runtime_anchor<U>, task<T, U>);

 public:
  /** @brief 创建暂未绑定目标的凭据；on 启动时会拒绝。 */
  basic_runtime_anchor() = default;

 private:
  explicit basic_runtime_anchor(std::shared_ptr<detail::runtime_anchor_state> state)
      : state_(std::move(state)) {}

  std::shared_ptr<detail::runtime_anchor_state> state_;
};

/** @brief 显式运行时所有者，只接受有独立驱动线程的目标。 */
class anchored_runtime {
 public:
  /** @brief 构造独立运行的多工作线程目标，并允许通过 anchor 提交任务。 */
  explicit anchored_runtime(::faio::runtime::config cfg = {})
      : context_(fixed_mode(std::move(cfg))) {
    anchor_state_->active = &context_;
  }

  anchored_runtime(const anchored_runtime&) = delete;
  anchored_runtime& operator=(const anchored_runtime&) = delete;

  /** @brief 在正常 runtime 析构排空前关闭准入。 */
  ~anchored_runtime() {
    const std::lock_guard lock{anchor_state_->mutex};
    anchor_state_->active = nullptr;
  }

  /** @brief 获取可传入 on 的凭据。持有凭据不会阻止已经开始的关闭。 */
  basic_runtime_anchor<> anchor() const noexcept { return basic_runtime_anchor<>{anchor_state_}; }

  /** @brief 启动并阻塞等待当前 runtime 内的用户任务。 */
  template <class T>
  T block_on(task<T> work) {
    return context_.block_on(std::move(work));
  }

  /** @brief 关闭跨域准入，然后按现有运行时协议请求停止或排空。 */
  void stop(io::shutdown_policy policy = io::shutdown_policy::drain) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("anchored runtime cannot be stopped synchronously from a worker");
    {
      const std::lock_guard lock{anchor_state_->mutex};
      anchor_state_->active = nullptr;
    }
    context_.stop(policy);
  }

 private:
  /** @brief 固定使用多工作线程模式，保留调用方的其他配置。 */
  static ::faio::runtime::config fixed_mode(::faio::runtime::config cfg) {
    cfg._mode = ::faio::runtime::mode::multi_thread;
    return cfg;
  }

  std::shared_ptr<detail::runtime_anchor_state> anchor_state_{
      std::make_shared<detail::runtime_anchor_state>()};
  detail::context_type context_;
};

/** @brief 在有独立驱动线程的目标运行子任务，值或异常在来源协程交付。 */
template <class S, class T>
task<T, S> on(basic_runtime_anchor<S> target, task<T, S> child) {
  if (!target.state_)
    throw std::logic_error("on requires a runtime anchor");
  const auto source = co_await this_coro::scheduler();
  bool same{};
  {
    const std::lock_guard lock{target.state_->mutex};
    if (!target.state_->active)
      throw std::logic_error("on target is closed");
    same = target.state_->active->scheduler() == source;
  }
  if (same) {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(child);
      co_return;
    } else {
      co_return co_await std::move(child);
    }
  }
  const auto policy = ::faio::detail::current_task_context().scheduling;
  if (policy.binds_core() || policy.no_stealing)
    throw std::logic_error("on cannot cross runtime with a restricted source task");
  if constexpr (std::is_void_v<T>) {
    co_await faio::make_awaiter(
        detail::on_operation<T, S>{std::move(target.state_), std::move(child)});
  } else {
    co_return co_await faio::make_awaiter(
        detail::on_operation<T, S>{std::move(target.state_), std::move(child)});
  }
}
}  // namespace faio::experimental

namespace faio {
using experimental::anchored_runtime;
using experimental::basic_runtime_anchor;
using experimental::on;
}  // namespace faio
