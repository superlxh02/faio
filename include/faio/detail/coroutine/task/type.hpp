/**
 * @file faio/detail/coroutine/task/type.hpp
 * @brief task 的唯一帧所有权与嵌套 awaiter，保持公开类型身份。
 */
#ifndef FAIO_DETAIL_COROUTINE_TASK_TYPE_HPP
#define FAIO_DETAIL_COROUTINE_TASK_TYPE_HPP

#include "faio/detail/coroutine/task/promise.hpp"
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace faio {
namespace detail {
/** @brief 选择清理阶段独立停止上下文的内部等待器。 */
template <class S>
struct cleanup_awaiter;

/** @brief 词法组主体的内部停止上下文交接。 */
template <class T, class S>
struct task_group_body_awaiter;

/** @brief 支持局部附加停止源的原生任务等待器。 */
template <class T, class S, bool Observe>
struct with_stop_token_awaiter;
}  // namespace detail

/**
 * @brief 独占用户任务帧的一次性惰性任务，只能移动，创建时保持挂起。
 * co_await 必须消费右值，等待或提交时须转移帧所有权。未启动的任务可安全销毁。
 * 已启动但仍挂起的任务不得从外部销毁，因为 io_uring 或等待队列可能仍借用帧内地址；
 * 必须等任务安全完成后，由 await_resume 销毁协程帧。
 */
template <class T, class S>
class [[nodiscard]] task {
 public:
  using scheduler_type = S;
  using value_type = T; /**< 公开结果类型，scope 等组合器据此推导返回值。 */
  using promise_type =
      detail::task_promise<T, S>; /**< 编译器协程协议使用的 promise 类型，负责结果和最终挂起。 */
  using handle_type =
      std::coroutine_handle<promise_type>; /**< 带具体 promise 类型的句柄，用于访问本任务帧状态。 */

  /** @brief 构造空任务，可作为移动后的空状态；空任务不能被有效提交。 */
  task() noexcept = default;

  /** @brief 接管新建协程帧的唯一所有权，不在这里执行协程。 */
  explicit task(handle_type h) noexcept : handle_(h) {}

  /** @brief 释放仍归本对象所有的帧；正常 await/spawn 后句柄已转出，不重复销毁。 */
  ~task() {
    if (handle_)
      handle_.destroy();
  }

  /** @brief 禁止复制同一个帧的所有权。 */
  task(const task&) = delete;

  /** @brief 禁止复制赋值，避免双重等待和双重销毁。 */
  task& operator=(const task&) = delete;

  /** @brief 接管帧并将源任务置空，移动过程不执行用户代码。 */
  task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  /** @brief 先释放旧的未转出帧，再接管源任务；本接口不承担取消已挂起任务的功能。 */
  task& operator=(task&& other) noexcept {
    if (this != &other) {
      if (handle_)
        handle_.destroy();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  /** @brief 父协程等待用户 task 时的帧所有者，生命周期覆盖整个子任务执行。 */
  struct awaiter : private detail::native_task_awaiter_tag {
    handle_type callee; /**< 被等待子协程的唯一句柄；await_resume 之后被交换为空。 */
    detail::task_scheduling_policy parent_scheduling{};

    /** @brief 从 task 接管子帧，暂不启动它。 */
    explicit awaiter(handle_type h) noexcept : callee(h) {}

    /** @brief 禁止复制子帧所有权。 */
    awaiter(const awaiter&) = delete;

    /** @brief 登记前转移等待者，源等待者不再销毁子帧。 */
    awaiter(awaiter&& other) noexcept
        : callee(std::exchange(other.callee, {})), parent_scheduling(other.parent_scheduling) {}

    /** @brief 清理仍持有的帧；正常恢复领取结果后 callee 已置空。 */
    ~awaiter() {
      if (callee)
        callee.destroy();
    }

    /** @brief 挂起前检查：空任务直接进入恢复阶段报告错误；有效惰性任务需要启动。 */
    bool await_ready() const noexcept { return !callee; }

    /** @brief 登记挂起协程的恢复路径。 */
    template <class Parent>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) {
      return await_suspend_impl(parent);
    }

    /** @brief 恢复时：领取子任务结果或重抛异常，并保证两条路径均销毁子帧。 */
    T await_resume() {
      if (!callee)
        throw std::logic_error("co_await 空 task");

      ::faio::detail::current_thread_context().task_ctx.scheduling = parent_scheduling;
      return await_resume_impl();
    }

   private:
    template <class, class>
    friend struct detail::scoped_awaiter;

    /** @brief 原生上下文包装器经父 promise 恢复策略，无需重复保存。 */
    template <class Parent>
    std::coroutine_handle<> await_suspend_scoped(std::coroutine_handle<Parent> parent) {
      return await_suspend_impl<false>(parent);
    }

    /** @brief 直接从父 promise 恢复上下文后领取子任务结果。 */
    T await_resume_scoped(const detail::task_context& context) {
      ::faio::detail::restore_native_task_context(context);
      if (!callee)
        throw std::logic_error("co_await 空 task");
      return await_resume_impl();
    }

    /** @brief 消费有效子任务的结果，并在正常或异常退出时回收子帧。 */
    T await_resume_impl() {
      struct frame_guard {
        handle_type handle; /**< 从 awaiter 转入守卫的唯一子帧句柄。 */

        /** @brief 离开 await_resume 时销毁子帧，包括异常展开路径。 */
        ~frame_guard() { handle.destroy(); }

      } guard{std::exchange(callee, {})};

      if constexpr (std::is_void_v<T>)
        guard.handle.promise().take_result();
      else
        return guard.handle.promise().take_result();
    }

    friend struct detail::cleanup_awaiter<S>;
    friend struct detail::task_group_body_awaiter<T, S>;
    friend struct detail::with_stop_token_awaiter<T, S, false>;
    friend struct detail::with_stop_token_awaiter<T, S, true>;

    template <class stop_owner_type>
    static void set_child_stop_owner(detail::task_context& context, const stop_owner_type& owner) {
      context.stop_token = owner->source.get_token();
      context.cancellation_owner = owner;
    }

    /**
     * @brief 安装父续体和子任务上下文，返回 await_suspend 所需的转移目标。
     * 通常直接返回子句柄；跨绑核 worker 时投递子任务并返回 noop。
     * 显式停止上下文在发布子任务前安装。
     */
    template <bool save_parent_scheduling = true, class Parent, class... stop_owner_types>
    std::coroutine_handle<> await_suspend_impl(std::coroutine_handle<Parent> parent,
                                               const stop_owner_types&... stop_owner) {
      static_assert(sizeof...(stop_owner_types) <= 1);
      if constexpr (requires { typename Parent::scheduler_type; }) {
        static_assert(std::same_as<S, typename Parent::scheduler_type>,
                      "父子 task 的静态调度器类型必须一致");
      }
      const auto* execution = ::faio::detail::current_execution_binding();
      auto scheduler =
          execution ? execution->template try_scheduler<S>() : basic_scheduler_ref<S>{};
      if (!scheduler) [[unlikely]]
        scheduler = ::faio::detail::require_current_scheduler_for<S>();

      auto& child = callee.promise();
      child.continuation = parent;
      if constexpr (requires { parent.promise().context; }) {
        static_assert(std::same_as<std::remove_cvref_t<decltype(parent.promise().context)>,
                                   detail::task_context>,
                      "父协程的任务上下文类型不兼容");
        child.context = parent.promise().context;
      } else {
        child.context = ::faio::detail::capture_task_context();
      }
      if constexpr (save_parent_scheduling)
        parent_scheduling = child.context.scheduling;
      if constexpr (sizeof...(stop_owner_types) == 1)
        set_child_stop_owner(child.context, stop_owner...);
      child.initialize_scheduling_opt(child.context.scheduling);
      const auto scheduling = child.context.scheduling;
      if (scheduling.binds_core()
          && scheduling.bound_worker != ::faio::detail::current_worker_id()) {
        const detail::basic_resume_target<S> target{scheduler, scheduling};
        target.schedule(callee);
        return std::noop_coroutine();
      }
      return callee;
    }
  };

  /** @brief 右值等待消费 task 所有权，awaiter 接管帧，原 task 变为空。 */
  awaiter operator co_await() && noexcept { return awaiter{std::exchange(handle_, {})}; }

  /** @brief 禁止隐式等待左值 task，调用者须 std::move 明确转交一次性所有权。 */
  awaiter operator co_await() & = delete;

  /** @brief 本任务及后代禁止窃取；不改变父任务或兄弟任务的策略。 */
  task with_no_stealing() && {
    scheduling_options().stealing = detail::scheduling_setting::DISABLED;
    return std::move(*this);
  }

  /** @brief 显式允许窃取；若祖先已禁止，启动时告警并保留继承的限制。 */
  task with_stealing() && {
    scheduling_options().stealing = detail::scheduling_setting::ENABLED;
    return std::move(*this);
  }

  /** @brief 首次执行绑定当前 worker 的已验证 CPU，自动禁止本任务及后代被窃取。 */
  task with_bind_core() && {
    scheduling_options().bind_core = detail::scheduling_setting::ENABLED;
    return std::move(*this);
  }

  /** @brief 显式不绑核；不能解除祖先约束，也不会自动开启窃取。 */
  task with_no_bind_core() && {
    scheduling_options().bind_core = detail::scheduling_setting::DISABLED;
    return std::move(*this);
  }

  /**
   * @brief 按值持有静态策略，首次执行前调用 before，正常或异常退出后调用 after。
   * 包装增加一层惰性协程帧，保持结果和 Scheduler 类型；空任务立即报告错误。
   */
  template <class Policy>
    requires task_lifecycle_policy<std::decay_t<Policy>>
             && std::constructible_from<std::decay_t<Policy>, Policy&&>
  task with_lifecycle(Policy&& policy) &&;

  /** @brief 使用一对普通可调用对象配置生命周期；after 必须接受 task_exit 且 noexcept。 */
  template <class Before, class After>
    requires detail::task_lifecycle_callback_pair<std::decay_t<Before>, std::decay_t<After>>
             && std::constructible_from<std::decay_t<Before>, Before&&>
             && std::constructible_from<std::decay_t<After>, After&&>
  task with_lifecycle(Before&& before, After&& after) &&;

  /** @brief 低层提交接口：取走唯一句柄；调用者此后承担执行和销毁责任。 */
  handle_type take() {
    if (!handle_)
      throw std::logic_error("空 task 不能提交");
    return std::exchange(handle_, {});
  }

 private:
  /** @brief 读取任务调度属性。 */
  detail::task_scheduling_options& scheduling_options() {
    if (!handle_)
      throw std::logic_error("不能为空 task 配置调度属性");
    if (handle_.promise().scheduling_initialized)
      throw std::logic_error("不能为已启动 task 修改调度属性");
    return handle_.promise().scheduling_options;
  }

  handle_type handle_{}; /**< 尚未等待或提交的任务帧句柄，空值表示已转出或未绑定任务。 */
};

/** @brief 创建协程时：由有结果 promise 产生其用户 task 对象。 */
template <class T, class S>
inline task<T, S> detail::task_promise<T, S>::get_return_object() noexcept {
  return task<T, S>{std::coroutine_handle<task_promise<T, S>>::from_promise(*this)};
}

/** @brief 创建协程时：由 void promise 产生对应用户任务。 */
template <class S>
inline task<void, S> detail::task_promise<void, S>::get_return_object() noexcept {
  return task<void, S>{std::coroutine_handle<task_promise<void, S>>::from_promise(*this)};
}
}  // namespace faio
#endif
