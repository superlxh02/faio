/**
 * @file faio/detail/coroutine/root_task.hpp
 * @brief 协程帧、任务上下文与并发组合：根协程与计数交接。
 * @details 协程句柄的拥有、借用及转交由各类型明确约定。结果发布、停止传播和任务组计数须覆盖挂起、恢复及异常退出，发布恢复句柄后不再访问可能已销毁的帧。
 */
#ifndef FAIO_DETAIL_COROUTINE_ROOT_TASK_HPP
#define FAIO_DETAIL_COROUTINE_ROOT_TASK_HPP

#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/coroutine/task_context.hpp"
#include "faio/detail/common/cancellation.hpp"
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <utility>

namespace faio::detail {
/// @brief 调度器拥有的根协程与用户 task 分开管理：根协程结束时自动销毁帧。
/// promise_type 必须在协程函数定义处完整，因此仅前置声明 detached_task 不够。
/// 调度器接管的根协程，与用户 task 的“父 awaiter 销毁帧”模式分开。
/// 提交前本对象拥有帧，提交后帧在 final_suspend 的不挂起路径自动销毁。
class detached_task {
 public:
  /// @brief 根协程 promise，保存结束时归还计数所需的借用目标，不保存用户结果。
  struct promise_type {
    /// @brief 根帧销毁时归还外层 tracker 和运行时计数，提交失败清理也走此入口。
    ~promise_type() {
      /// @brief 先归还外层任务组的一票，例如解除 block_on 的排空等待。
      auto completion = scope ? scope->done() : task_tracker::completion_guard{};
      /// @brief 归还运行时活动根任务计数，使运行时能判断何时可以关闭 worker。
      if (lifetime)
        /// @brief 子根帧已完成清理，归还运行时活动任务计数，关闭方才能继续排空。
        lifetime.finish_task();
      /// @brief 最后归还外部任务图子任务租约，保证 promise 清理尾段仍受宿主计数保护。
      external_child.reset();
    }

    task_tracker* scope{};  ///< 可选外层任务计数器，只借用；计数登记必须与此析构中的 done 配对。
    task_lifetime_ref lifetime{};  ///< 独立的根任务生命周期服务，登记成功后才绑定，析构时归还一票。
    external_child_lease external_child;  ///< 外部任务图的子任务租约，覆盖当前根协程的执行和清理。

    /// @brief 创建协程时：生成提交前拥有根帧的 detached_task 对象。
    detached_task get_return_object() noexcept {
      /// @brief 由 promise 定位根协程帧，此时尚未投递执行。
      return detached_task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    /// @brief 首次挂起时：根包装保持惰性，等 start_detached 登记后才投递。
    std::suspend_always initial_suspend() const noexcept { return {}; }

    /// @brief 最终挂起时：不保留根帧，结束后自动销毁并归还任务计数。
    std::suspend_never final_suspend() const noexcept { return {}; }

    /// @brief 执行无值 co_return 或运行到末尾时：正常结束，不保存用户返回值。
    void return_void() const noexcept {}

    /// @brief 异常离开根协程体时：没有父任务接收异常，明确终止进程。
    void unhandled_exception() const noexcept { std::terminate(); }
  };

  using handle_type = std::coroutine_handle<promise_type>;  ///< 根 promise 对应的强类型帧句柄，提交入口通过它设置清理目标。

  /// @brief 接管新建的尚未提交根帧，不立即执行。
  explicit detached_task(handle_type h) : handle_(h) {}

  /// @brief 移动根帧所有权并把源对象置空，保证仅一个提交前持有者。
  detached_task(detached_task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  /// @brief 禁止复制根帧句柄，避免重复提交与销毁。
  detached_task(const detached_task&) = delete;

  /// @brief 销毁尚未交给调度器的根帧；已 take 转出的帧由执行结束负责销毁。
  ~detached_task() {
    if (handle_)
      /// @brief 释放尚未移交给等待器或调度器的协程帧，不能用于强制取消仍被借用的活动帧。
      handle_.destroy();
  }

  /// @brief 向提交入口转出唯一根帧所有权，本对象之后不负责销毁。
  handle_type take() noexcept { return std::exchange(handle_, {}); }

  /// @brief 投递前绑定根帧析构时的计数归还目标，必须与调用者的登记配对。
  void set_completion(task_lifetime_ref lifetime, task_tracker* scope = nullptr) noexcept {
    /// @brief 保存运行时根任务计数的归还接口。
    handle_.promise().lifetime = lifetime;
    /// @brief 保存外层任务组的归还目标，无外层任务组时为空。
    handle_.promise().scope = scope;
  }
  void set_external_child(external_child_lease child) noexcept {
    /// @brief 把外部图子任务责任移入根 promise，根帧析构在所有退出路径配对归还。
    handle_.promise().external_child = std::move(child);
  }

 private:
  handle_type handle_;  ///< 提交前独占的根帧句柄；转出后为空，避免对象析构重复清理。
};

/// @brief 登记并立即投递根协程，成功后调度器拥有它的执行生命周期。
/// 调用前外层 scope 计数已登记；本函数保证投递失败也通过根帧析构归还。
inline void start_detached(detached_task root,
                           scheduler_ref scheduler,
                           task_tracker* scope = nullptr,
                           task_lifetime_ref lifetime = current_task_lifetime()) {
  /// @brief 即使没有运行时，调用者已登记的作用域计数也要由帧析构归还。
  /// 先绑定清理目标，后续异常销毁 root 时仍能归还外层计数。
  root.set_completion({}, scope);
  /// @brief 提交前捕获当前外部任务图归属，后续线程上下文切换不改变本次接纳身份。
  const auto external_scope = current_external_scope;
  if (external_scope.valid() && external_scope.host == current_external_host()
      && scheduler == current_scheduler())
    /// @brief 在发布根句柄前取得同宿主子任务接纳租约，避免父根结束后派生工作脱离排空计数。
    root.set_external_child(external_scope.host->acquire_child(external_scope));
  /// @brief 空运行时无法提交，root 的析构会清理尚未执行的根帧。
  if (!scheduler)
    throw std::logic_error("没有可用的运行时");
  /// @brief 投递前给运行时活动根任务增加一票，避免 shutdown 漏掉快速任务。
  lifetime.register_task();
  /// @brief 登记成功后才让 promise 负责归还；空调度器失败不会归还未登记的计数。
  root.set_completion(lifetime, scope);
  /// @brief 转出根帧所有权，随后成功交给调度器或在失败分支手动清理。
  auto handle = root.take();
  try {
    /// @brief 调度器选择本地/全局入队；根帧可立即在其他 worker 开始执行。
    scheduler.schedule(handle);
  } catch (...) {
    /// @brief 入队失败时没有执行者接管帧，当前线程负责销毁并归还登记。
    /// @brief promise 析构会注销根任务与作用域计数。
    handle.destroy();
    /// @brief 把入队错误继续交给上层，让上层组合器排空已启动的其他任务。
    throw;
  }
}

/// @brief 无结果观察者的根包装，拥有 child，借用 tracker；异常由根 promise
/// 终止规则处理。
template <class T>
detached_task spawn_coro(task<T> child, task_tracker* tracker,
                         std::stop_token stop = current_stop_token,
                         cancellation_error_policy policy = current_cancellation_policy,
                         external_scope_ref external_scope = current_external_scope,
                         std::shared_ptr<cancellation_state> cancellation_owner = current_cancellation_owner) {
  /// @brief 没有 join 接收者的后台任务若抛异常，则终止程序，避免静默丢失。
  /// 启动用户 task 前安装任务组，内部 spawn 继承这个归属。
  current_tracker = tracker;
  /// @brief 根包装安装提交时捕获的停止令牌，首个用户 task 从该入口继承取消状态。
  current_stop_token = stop;
  /// @brief 安装本次根提交约定的未观察取消处理策略。
  current_cancellation_policy = policy;
  /// @brief 在根包装入口恢复捕获的外部任务图身份，子任务继续派生时沿用正确宿主。
  current_external_scope = external_scope;
  /// @brief 将取消连接租约移入当前根执行上下文，保持父级传播链覆盖子任务执行。
  current_cancellation_owner = std::move(cancellation_owner);
  /// @brief 消费并等待用户 task；结果在此丢弃，但异常不能静默丢弃。
  try {
    co_await std::move(child);
  } catch (const operation_cancelled&) {
    if (policy != cancellation_error_policy::normal_if_stop_requested || !stop.stop_requested())
      throw;
  }
}

/// @brief 立即提交不需要结果句柄的任务，供 spawn_detached 使用。
template <class T>
void start_unobserved(scheduler_ref scheduler,
                      task<T> child,
                      task_tracker* tracker = nullptr,
                      task_lifetime_ref lifetime = current_task_lifetime()) {
  /// @brief 先构造包装帧，构造失败时尚未增加外层计数。
  const bool inherit = !current_scheduler() || scheduler == current_scheduler();
  auto root = spawn_coro(std::move(child), tracker,
      inherit ? current_stop_token : std::stop_token{},
      inherit ? current_cancellation_policy : cancellation_error_policy::fatal_if_unobserved,
      inherit ? current_external_scope : external_scope_ref{},
      inherit ? current_cancellation_owner : nullptr);
  /// @brief 外层任务组登记一票，根 promise 析构负责配对归还。
  if (tracker)
    /// @brief 投递前登记外层任务组计数，执行结束或提交失败的根帧析构负责归还。
    tracker->add();
  /// @brief 把包装交给运行时登记并入队，无 JoinHandle 共享结果分配。
  start_detached(std::move(root), scheduler, tracker, lifetime);
}
}

#endif
