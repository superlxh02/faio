/**
 * @file faio/detail/coroutine/lifecycle/owned_hooks.hpp
 * @brief 由 task promise 拥有的两阶段生命周期辅助帧实现。
 */
#ifndef FAIO_DETAIL_COROUTINE_LIFECYCLE_OWNED_HOOKS_HPP
#define FAIO_DETAIL_COROUTINE_LIFECYCLE_OWNED_HOOKS_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/lifecycle/policy.hpp"
#include <concepts>
#include <coroutine>
#include <exception>
#include <utility>

namespace faio::detail {
/** @brief 注解辅助帧拥有的状态；未注解任务不为这些字段增加帧空间。 */
struct task_lifecycle_state {
  /** @brief 辅助帧只允许首次执行与最终退出两次恢复。 */
  enum class phase : unsigned char { UNSTARTED, BEFORE_DONE, AFTER_DONE };

  task_exit exit{};              /**< after 观察的最终状态，不保存任务结果。 */
  phase stage{phase::UNSTARTED}; /**< 用于验证两阶段恢复顺序。 */
  bool before_failed{};          /**< before 的异常统一归为失败，不误判成任务取消。 */
  bool writing_result{};         /**< promise 构造返回值失败时统一归为失败。 */
};

/** @brief 拥有标准协程句柄；具体 Policy 位于辅助帧中，before/after 保持静态调用。 */
class owned_task_lifecycle {
 public:
  /** @brief 协程返回值与挂起协议。 */
  struct promise_type {
    task_lifecycle_state state{}; /**< 状态与具体策略一同由辅助帧拥有，原 task 只保存句柄。 */

    /** @brief 从辅助 promise 建立唯一拥有者，不将具体 Policy 擦成借用回调表。 */
    owned_task_lifecycle get_return_object() noexcept {
      return owned_task_lifecycle{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    /** @brief 协程创建后保持挂起，等待显式启动。 */
    std::suspend_always initial_suspend() const noexcept { return {}; }

    /** @brief 完成后保留协程帧，等待拥有者销毁。 */
    std::suspend_always final_suspend() const noexcept { return {}; }

    /** @brief 标记无返回值协程完成。 */
    void return_void() const noexcept {}

    /** @brief 未捕获异常时终止进程。 */
    void unhandled_exception() const noexcept { std::terminate(); }
  };

  /** @brief 构造拥有型任务生命周期。 */
  owned_task_lifecycle() noexcept = default;
  /** @brief 禁止复制构造拥有型任务生命周期。 */
  owned_task_lifecycle(const owned_task_lifecycle&) = delete;
  /** @brief 禁止复制赋值。 */
  owned_task_lifecycle& operator=(const owned_task_lifecycle&) = delete;

  /** @brief 转交唯一辅助帧；源对象不会再次销毁。 */
  owned_task_lifecycle(owned_task_lifecycle&& other) noexcept
      : handle_(std::exchange(other.handle_, {})) {}

  /** @brief 无注解路径为空拥有者，不分配辅助帧。 */
  explicit operator bool() const noexcept { return static_cast<bool>(handle_); }

  /** @brief 未启动原 task 的销毁只释放辅助帧，不执行 before 或 after。 */
  ~owned_task_lifecycle() {
    if (handle_)
      handle_.destroy();
  }

  /** @brief 第一次恢复具体策略帧，随后在原协程的 initial await_resume 传播 before 异常。 */
  void before() {
    if (!handle_)
      return;
    auto& state = handle_.promise().state;
    if (state.stage != task_lifecycle_state::phase::UNSTARTED)
      std::terminate();
    handle_.resume();
    if (state.stage != task_lifecycle_state::phase::BEFORE_DONE)
      std::terminate();
    if (state.before_failed)
      std::rethrow_exception(state.exit.exception);
  }

  /** @brief 原任务最终挂起前第二次恢复；返回值已经写入 promise，任务体局部对象已清理。 */
  void after() noexcept {
    if (!handle_)
      return;
    auto& state = handle_.promise().state;
    if (state.stage != task_lifecycle_state::phase::BEFORE_DONE)
      std::terminate();
    handle_.resume();
    if (state.stage != task_lifecycle_state::phase::AFTER_DONE)
      std::terminate();
  }

  /** @brief 返回值构造开始后发生的异常属于失败，即使其类型表示协作取消。 */
  void begin_result() noexcept {
    if (handle_)
      handle_.promise().state.writing_result = true;
  }

  /** @brief 保存原任务异常；仅任务体直接传播的取消异常归类为 cancelled。 */
  void record_exception(std::exception_ptr exception) noexcept {
    if (!handle_)
      return;
    auto& state = handle_.promise().state;
    state.exit = {task_exit::kind::FAILED, std::move(exception)};
    if (state.before_failed || state.writing_result)
      return;
    try {
      std::rethrow_exception(state.exit.exception);
    } catch (const operation_cancelled&) {
      state.exit.status = task_exit::kind::CANCELLED;
    } catch (...) {
    }
  }

 private:
  /** @brief 构造拥有型任务生命周期。 */
  explicit owned_task_lifecycle(std::coroutine_handle<promise_type> handle) noexcept
      : handle_(handle) {}

  std::coroutine_handle<promise_type> handle_{}; /**< 固定布局的拥有句柄，空值不分配辅助帧。 */
};

/** @brief 原 promise 构造时使用一次的工厂；不作为任务借用引用或生命周期分派表保存。 */
using task_lifecycle_factory = owned_task_lifecycle (*)();

/** @brief 普通核心可理解的构造描述；反射只在调用端生成这个固定类型的值。 */
struct task_lifecycle_configuration {
  task_lifecycle_factory factory{};
};

/** @brief 在辅助协程内部立即读取自身状态，不排队、不额外创建协程帧。 */
struct task_lifecycle_state_query {
  task_lifecycle_state* state{};

  /** @brief 检查等待条件是否已满足。 */
  bool await_ready() const noexcept { return false; }

  /** @brief 返回 false 立即继续，状态与此协程帧的生命周期一致。 */
  bool await_suspend(std::coroutine_handle<owned_task_lifecycle::promise_type> handle) noexcept {
    state = &handle.promise().state;
    return false;
  }

  /** @brief 取得等待结果。 */
  task_lifecycle_state& await_resume() const noexcept { return *state; }
};

/** @brief 一个辅助帧拥有一个具体策略及状态，按原任务的初始/最终恢复分成两个阶段。 */
template <task_lifecycle_policy Policy>
owned_task_lifecycle make_task_lifecycle_frame(Policy policy) {
  auto& state = co_await task_lifecycle_state_query{};
  try {
    policy.before();
  } catch (...) {
    state.before_failed = true;
    state.exit = {task_exit::kind::FAILED, std::current_exception()};
  }
  state.stage = task_lifecycle_state::phase::BEFORE_DONE;
  co_await std::suspend_always{};
  policy.after(state.exit);
  state.stage = task_lifecycle_state::phase::AFTER_DONE;
}

/** @brief 注解选择策略类型，每次创建原 task 时默认构造独立的运行期策略。 */
template <task_lifecycle_policy Policy>
  requires std::default_initializable<Policy>
owned_task_lifecycle make_default_task_lifecycle() {
  return make_task_lifecycle_frame(Policy{});
}

}  // namespace faio::detail
#endif
