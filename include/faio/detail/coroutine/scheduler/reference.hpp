/**
 * @file faio/detail/coroutine/scheduler/reference.hpp
 * @brief 基于模板、CRTP 与显式对象参数的静态协程调度接口。
 */
#ifndef FAIO_DETAIL_COROUTINE_SCHEDULER_REF_HPP
#define FAIO_DETAIL_COROUTINE_SCHEDULER_REF_HPP

#include <concepts>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace faio::runtime::detail {
/** @brief I/O 工作线程唤醒接口。 */
class io_worker_waker;
/** @brief I/O 域关联的静态调度器。 */
template <class W>
class basic_domain_scheduler;
using domain_scheduler = basic_domain_scheduler<io_worker_waker>;
}  // namespace faio::runtime::detail

namespace faio {
/** @brief 内置运行时共享具体调度算法；驱动线程数由运行时模式决定。 */
using default_scheduler = runtime::detail::domain_scheduler;

namespace detail {
/** @brief 主动让出执行权的等待器。 */
template <class S>
struct basic_yield_awaiter;
/** @brief 按共享预算让出执行权的等待器。 */
template <class S>
struct basic_yield_if_needed_awaiter;
using yield_awaiter = basic_yield_awaiter<default_scheduler>;
using yield_if_needed_awaiter = basic_yield_if_needed_awaiter<default_scheduler>;
}  // namespace detail

/** @brief 成功接管句柄后安排一次恢复；入队内不直接 resume，失败不接管句柄。 */
template <class S>
concept coroutine_scheduler = requires(S& scheduler, std::coroutine_handle<> handle) {
  { scheduler.enqueue(handle) } -> std::same_as<void>;
};

template <class S>
concept ready_sink = requires(S& sink, std::coroutine_handle<> handle) {
  { sink.enqueue_ready(handle) } -> std::same_as<void>;
};

/** @brief CRTP 调度接口；显式对象参数保留具体拥有者，所有能力在编译期选择。 */
template <class Derived>
class scheduler_api {
 protected:
  /** @brief 构造调度器协议基类。 */
  scheduler_api() = default;
  /** @brief 销毁调度器协议基类。 */
  ~scheduler_api() = default;

 public:
  /** @brief 提交普通就绪任务。 */
  void schedule(this const Derived& self, std::coroutine_handle<> handle) {
    self.object().enqueue(handle);
  }

  /** @brief 提交 I/O 完成后的就绪任务。 */
  void schedule_io(this const Derived& self, std::coroutine_handle<> handle) {
    auto& scheduler = self.object();
    if constexpr (requires {
                    { scheduler.enqueue_io(handle) } -> std::same_as<void>;
                  })
      scheduler.enqueue_io(handle);
    else
      scheduler.enqueue(handle);
  }

  /** @brief 重新提交主动让出的协程。 */
  void schedule_yield(this const Derived& self, std::coroutine_handle<> handle) {
    auto& scheduler = self.object();
    if constexpr (requires {
                    { scheduler.enqueue_yield(handle) } -> std::same_as<void>;
                  })
      scheduler.enqueue_yield(handle);
    else
      scheduler.enqueue(handle);
  }

 private:
  template <class S>
  friend struct detail::basic_yield_awaiter;
  template <class S>
  friend struct detail::basic_yield_if_needed_awaiter;

  /** @brief 只有实际主动让出的挂起点可以提供此提示；发布以后不得再访问 awaiter。 */
  void schedule_cooperative_yield(this const Derived& self, std::coroutine_handle<> handle) {
    auto& scheduler = self.object();
    if constexpr (requires {
                    { scheduler.enqueue_cooperative_yield(handle) } -> std::same_as<void>;
                  })
      scheduler.enqueue_cooperative_yield(handle);
    else
      self.schedule_yield(handle);
  }
};

/**
 * @brief 单指针静态调度借用，不拥有调度器，不保存操作表。
 * S 的 enqueue 必须遵守原有约定：跨线程安全，失败时不接管句柄，成功时只恢复一次。
 */
template <class S>
class basic_scheduler_ref : public scheduler_api<basic_scheduler_ref<S>> {
  static_assert(!std::is_void_v<S>, "Scheduler 必须是具体调度器类型");

 public:
  using scheduler_type = S;

  /** @brief 构造静态调度器引用。 */
  basic_scheduler_ref() noexcept = default;

  /**
   * @brief 保留 S& 接纳规则；构造模板仅在实际借用时检查完整调度器能力。
   * @details Complete 没有可供推导的形参，始终采用默认类型 S；默认构造或复制借用时，不提前检查尚未完整定义的 S 是否满足约束。
   */
  template <class Complete = S>
  explicit basic_scheduler_ref(S& scheduler) noexcept
    requires coroutine_scheduler<Complete>
      : state_(std::addressof(scheduler)) {}

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return state_ != nullptr; }

  /** @brief 借用底层原生句柄。 */
  S* native_handle() const noexcept { return state_; }

  /** @brief 线程绑定已校验类型后重建借用；不分配、不分派，也不要求此处 S 已完整。 */
  static basic_scheduler_ref from_native_handle(S* scheduler) noexcept {
    basic_scheduler_ref result;
    result.state_ = scheduler;
    return result;
  }

  /** @brief 比较两个对象的值。 */
  friend bool operator==(basic_scheduler_ref left, basic_scheduler_ref right) noexcept {
    return left.state_ == right.state_;
  }

 private:
  friend class scheduler_api<basic_scheduler_ref<S>>;

  /** @brief 借用存储中的对象。 */
  S& object() const {
    if (!state_)
      throw std::logic_error("没有可用的调度器");
    return *state_;
  }

  S* state_{};
};

namespace detail {
/** @brief task 的静态调度器身份独立于执行属性存储，类型匹配不依赖帧内保存调度器。 */
template <class Promise>
concept has_task_scheduler_type = requires {
  typename Promise::faio_task_promise_tag;
  typename Promise::scheduler_type;
  requires std::same_as<typename Promise::faio_task_promise_tag, void>;
};

template <class T>
inline constexpr bool is_static_scheduler_reference_v = false;
template <class S>
inline constexpr bool is_static_scheduler_reference_v<basic_scheduler_ref<S>> = true;
}  // namespace detail

/** @brief 默认引用同样只保存具体调度器指针；不存在动态操作表。 */
using scheduler_ref = basic_scheduler_ref<default_scheduler>;
template <class S>
using scheduler_ref_for = basic_scheduler_ref<S>;

/** @brief 类型与实例共同确定调度域，不存在跨类型的运行时分派。 */
template <class A, class B>
bool same_scheduler(basic_scheduler_ref<A> left, basic_scheduler_ref<B> right) noexcept {
  if constexpr (std::same_as<A, B>)
    return left == right;
  else
    return false;
}

}  // namespace faio
#endif
