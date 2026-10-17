/**
 * @file faio/detail/coroutine/execution_thread.hpp
 * @brief 协程帧、任务上下文与并发组合：执行线程绑定。
 * @details 协程句柄的拥有、借用及转交由各类型明确约定。结果发布、停止传播和任务组计数须覆盖挂起、恢复及异常退出，发布恢复句柄后不再访问可能已销毁的帧。
 */
#ifndef FAIO_DETAIL_COROUTINE_EXECUTION_THREAD_HPP
#define FAIO_DETAIL_COROUTINE_EXECUTION_THREAD_HPP

#include "faio/detail/coroutine/scheduler.hpp"
#include "faio/detail/coroutine/task_lifetime_ref.hpp"
#include <cstddef>
#include <limits>
#include <memory>

namespace faio::runtime::detail {
class blocking_pool;
}

namespace faio::detail {
class external_work_host;
/// @brief 没有工作线程编号时的统一哨兵；编号不是任务的永久属性。
inline constexpr std::size_t no_worker_id = std::numeric_limits<std::size_t>::max();

/// @brief 稳定的线程绑定对象：所属线程运行期间存放在线程栈上的 worker 成员内。
/// 调度器、生命周期服务、本地状态和编号在同一次构造中绑定，避免 TLS 状态漂移。
class execution_thread_binding {
 public:
  /// @brief 借用本地状态；类型标识保证后端只会取回当初绑定的真实类型。
  template <class local_state_type>
  execution_thread_binding(scheduler_ref scheduler,
                           task_lifetime_ref lifetime,
                           local_state_type& local_state,
                           std::size_t worker_id,
                           ::faio::runtime::detail::blocking_pool* blocking = nullptr,
                           external_work_host* external_host = nullptr) noexcept
      : scheduler_(scheduler),
        lifetime_(lifetime),
        local_state_(std::addressof(local_state)),
        local_type_(&type_tag<local_state_type>),
        worker_id_(worker_id),
        blocking_(blocking), external_host_(external_host) {}

  /// @brief 查询当前线程的通用调度入口，不暴露 worker 或 shared。
  scheduler_ref scheduler() const noexcept { return scheduler_; }

  /// @brief 查询当前线程所属的根任务生命周期服务。
  task_lifetime_ref lifetime() const noexcept { return lifetime_; }

  /// @brief 查询本次执行线程的编号；任务迁移后应重新查询。
  std::size_t worker_id() const noexcept { return worker_id_; }

  /// @brief 取得文件或通用阻塞执行服务引用，具体服务归属由上下文决定。
  ::faio::runtime::detail::blocking_pool* blocking() const noexcept { return blocking_; }
  external_work_host* external_host() const noexcept { return external_host_; }

  /// @brief 同时校验调度域身份和本地状态类型，避免对任意后端执行盲目转换。
  template <class local_state_type>
  local_state_type* local_state_for(scheduler_ref target) const noexcept {
    if (scheduler_ != target || local_type_ != &type_tag<local_state_type>)
      return nullptr;
    return static_cast<local_state_type*>(local_state_);
  }

 private:
  /// @brief 每个本地状态类型拥有唯一地址，只用于身份比较，不需要 RTTI。
  template <class T>
  static inline const unsigned char type_tag{};
  scheduler_ref scheduler_;  ///< 当前线程所属调度域的借用入口。
  task_lifetime_ref lifetime_;  ///< 所属 runtime 的根任务计数服务。
  void* local_state_;  ///< 借用具体后端的本地状态。
  const void* local_type_;  ///< 与本地状态真实类型匹配的静态标识。
  std::size_t worker_id_;  ///< 当前线程编号；无编号使用 no_worker_id。
  ::faio::runtime::detail::blocking_pool* blocking_{};  ///< 借用运行时通用阻塞任务池，避免在协程工作线程执行同步阻塞调用。
  external_work_host* external_host_{};  ///< 借用外部任务宿主，接纳后代任务并维持任务图排空协议。
};

/// @brief 调度相关的唯一 TLS 入口；任务 tracker 和 stop_token 仍随每次任务恢复切换。
inline thread_local const execution_thread_binding* current_execution_thread{};

struct submission_binding {
  scheduler_ref scheduler;  ///< 捕获的目标调度器，跨线程完成时将续体投递回所属运行时。
  task_lifetime_ref lifetime;  ///< 根任务计数服务的借用引用，接纳与完成必须成对记账。
  ::faio::runtime::detail::blocking_pool* blocking{};  ///< 借用运行时通用阻塞任务池，避免在协程工作线程执行同步阻塞调用。
  external_work_host* external_host{};  ///< 借用外部任务宿主，接纳后代任务并维持任务图排空协议。
};
inline thread_local const submission_binding* current_submission_binding{};

/// @brief 普通阻塞池线程可提交后代任务，无需成为调度工作线程；阻塞接口与 worker 编号检查保持原语义。
class scoped_submission_binding {
 public:
  scoped_submission_binding(scheduler_ref scheduler, task_lifetime_ref lifetime,
                            ::faio::runtime::detail::blocking_pool& blocking,
                            external_work_host* host) noexcept
      : binding_{scheduler, lifetime, &blocking, host}, previous_(current_submission_binding) {
    /// @brief 安装只提供提交能力的线程局部绑定，普通阻塞线程仍不被标记为调度工作线程。
    current_submission_binding = &binding_;
  }
  scoped_submission_binding(const scoped_submission_binding&) = delete;
  scoped_submission_binding& operator=(const scoped_submission_binding&) = delete;
  ~scoped_submission_binding() { current_submission_binding = previous_; }
 private:
  submission_binding binding_;  ///< 当前作用域安装的线程提交或执行绑定，退出时恢复外层状态。
  const submission_binding* previous_;  ///< 进入当前作用域前保存的线程绑定或任务上下文，析构时恢复。
};

/// @brief 线程进入/退出时成对安装绑定；支持嵌套安装后恢复原绑定，不分配内存。
class execution_thread_guard {
 public:
  /// @brief 安装稳定的借用对象，返回路径由析构统一恢复。
  explicit execution_thread_guard(const execution_thread_binding& binding) noexcept
      : previous_(current_execution_thread) {
    /// @brief 安装真实工作线程执行绑定，调度器据此核对本地状态与所属域身份。
    current_execution_thread = &binding;
  }

  /// @brief 禁止复制守卫，避免两个析构者恢复同一次线程绑定。
  execution_thread_guard(const execution_thread_guard&) = delete;

  execution_thread_guard& operator=(const execution_thread_guard&) = delete;

  /// @brief 离开线程执行范围时恢复此前绑定，避免留下悬空 TLS 指针。
  ~execution_thread_guard() { current_execution_thread = previous_; }

 private:
  const execution_thread_binding* previous_;  ///< 借用进入当前范围前的线程绑定。
};

/// @brief 读取当前调度器；普通外部线程返回空引用，不访问默认运行时单例。
inline scheduler_ref current_scheduler() noexcept {
  return current_execution_thread ? current_execution_thread->scheduler()
       : current_submission_binding ? current_submission_binding->scheduler : scheduler_ref{};
}

/// @brief 读取可选的根任务生命周期服务，供 worker 内派生任务继承。
inline task_lifetime_ref current_task_lifetime() noexcept {
  return current_execution_thread ? current_execution_thread->lifetime()
       : current_submission_binding ? current_submission_binding->lifetime : task_lifetime_ref{};
}

/// @brief 动态读取线程编号，任务挂起前保存的编号不能代表恢复后的线程。
inline std::size_t current_worker_id() noexcept {
  return current_execution_thread ? current_execution_thread->worker_id() : no_worker_id;
}

/// @brief 已绑定的执行线程禁止调用阻塞入口，防止占住调度线程造成死锁。
inline bool on_runtime_worker() noexcept {
  return current_execution_thread != nullptr;
}

inline ::faio::runtime::detail::blocking_pool* current_blocking_pool() noexcept {
  return current_execution_thread ? current_execution_thread->blocking()
       : current_submission_binding ? current_submission_binding->blocking : nullptr;
}
inline external_work_host* current_external_host() noexcept {
  return current_execution_thread ? current_execution_thread->external_host()
       : current_submission_binding ? current_submission_binding->external_host : nullptr;
}
}
#endif
