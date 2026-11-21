/**
 * @file thread_context.hpp
 * @brief 当前执行线程的服务借用、任务镜像与执行状态。
 */
#ifndef FAIO_DETAIL_COROUTINE_THREAD_CONTEXT_HPP
#define FAIO_DETAIL_COROUTINE_THREAD_CONTEXT_HPP

#include "faio/detail/coroutine/context/task_context.hpp"
#include "faio/detail/coroutine/memory/large_frame_pool.hpp"
#include <optional>
#include <cstdint>

namespace faio::io::detail {
/** @brief I/O 资源与操作的归属域。 */
class io_domain;
}  // namespace faio::io::detail

namespace faio::runtime::detail {
/** @brief I/O 提交与驱动引擎。 */
class io_engine;

namespace timer {
/** @brief 运行时定时器。 */
class Timer;
}  // namespace timer
}  // namespace faio::runtime::detail

namespace faio::detail {
/** @brief 执行线程服务绑定基类。 */
class execution_thread_binding_base;
/** @brief 外部提交服务绑定基类。 */
class submission_binding_base;
/** @brief 线程本地协程帧缓存。 */
class coroutine_frame_cache;

inline constexpr std::uint16_t cooperative_budget_limit = 64;

/** @brief 执行线程上下文。 */
struct thread_context;
/** @brief 线程上下文的作用域绑定。 */
class scoped_thread_context;
/** @brief 取得当前执行线程上下文。 */
inline thread_context& current_thread_context() noexcept;

/** @brief 通过平凡 TLS 指针标记上下文是否存活；任务上下文析构后的协程帧回收只能查询此借用指针。 */
inline thread_context*& thread_context_instance_slot() noexcept {
  static thread_local thread_context* instance{};
  return instance;
}

/**
 * @brief 统一保存执行上下文的 TLS；服务指针均为借用，task_ctx 保存活跃任务上下文的值副本。
 * @details 各分区分别由所属入口安装和恢复，不能整体回滚此对象。
 *          挂起任务属性以 promise 为准；预算只属于本轮执行链，不随任务继承。
 */
struct thread_context {
 private:
  /** @brief 线程上下文激活凭证。 */
  struct activation_key {
   private:
    /** @brief 取得当前执行线程上下文。 */
    friend thread_context& current_thread_context() noexcept;
    friend class scoped_thread_context;
    /** @brief 构造线程上下文激活凭证。 */
    activation_key() noexcept = default;
  };

  /** @brief 取得当前执行线程上下文。 */
  friend thread_context& current_thread_context() noexcept;
  friend class scoped_thread_context;

 public:
  /** @brief 只有持有内部构造凭证的常规 TLS 和同步清理作用域可以登记实例。 */
  explicit thread_context(activation_key) noexcept { thread_context_instance_slot() = this; }

  /** @brief 禁止复制构造执行线程上下文。 */
  thread_context(const thread_context&) = delete;
  /** @brief 禁止复制赋值。 */
  thread_context& operator=(const thread_context&) = delete;

  /** @brief 先撤销存活标记，再由 task_ctx 成员释放停止状态和取消链。 */
  ~thread_context() {
    if (thread_context_instance_slot() == this)
      thread_context_instance_slot() = nullptr;
  }

  /** @brief 线程服务引用集合。 */
  struct service_bindings {
    const execution_thread_binding_base* execution{};
    const submission_binding_base* submission{};
  } services;

  /** @brief 线程的 I/O 服务引用集合。 */
  struct io_bindings {
    ::faio::runtime::detail::io_engine* engine{};
    ::faio::io::detail::io_domain* domain{};
    ::faio::runtime::detail::timer::Timer* timer{};
    ::faio::io::detail::io_domain* driver_domain{};
  } io;

  /** @brief 线程协程帧分配服务引用集合。 */
  struct frame_bindings {
    coroutine_frame_cache* cache{};
    large_frame_pool* large_pool{};
  } frames;

  task_context task_ctx{};

  /** @brief 协作轮询状态。 */
  struct poll_state {
    std::uint16_t remaining{cooperative_budget_limit};
  } poll;
};

/** @brief 查询活跃上下文，未找到时返回空，不构造新上下文，也不访问已析构的任务上下文。 */
inline thread_context* try_current_thread_context() noexcept {
  return thread_context_instance_slot();
}

/**
 * @brief 无活跃线程上下文时，临时安装栈上的实例，支持常规 TLS 析构后的同步清理。
 * @details 已有上下文时不构造实例或切换任何分区；不能跨 co_await 保存此守卫。
 */
class scoped_thread_context {
 public:
  /** @brief 构造线程上下文的作用域绑定。 */
  scoped_thread_context() noexcept {
    if (!try_current_thread_context())
      context_.emplace(thread_context::activation_key{});
  }

  /** @brief 禁止复制构造线程上下文的作用域绑定。 */
  scoped_thread_context(const scoped_thread_context&) = delete;
  /** @brief 禁止复制赋值。 */
  scoped_thread_context& operator=(const scoped_thread_context&) = delete;

 private:
  std::optional<thread_context> context_;
};

/** @brief 仅算法私有轮转游标保留各自的 TLS，执行上下文从此入口访问。 */
inline thread_context& current_thread_context() noexcept {
  if (auto* context = try_current_thread_context())
    return *context;
  static thread_local thread_context context{thread_context::activation_key{}};
  return context;
}
}  // namespace faio::detail
#endif
