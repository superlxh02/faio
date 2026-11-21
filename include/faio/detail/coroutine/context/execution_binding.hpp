/**
 * @file faio/detail/coroutine/context/execution_binding.hpp
 * @brief 执行线程的稳定服务绑定与具体调度器借用。
 */
#ifndef FAIO_DETAIL_COROUTINE_EXECUTION_THREAD_HPP
#define FAIO_DETAIL_COROUTINE_EXECUTION_THREAD_HPP

#include "faio/detail/coroutine/scheduler/reference.hpp"
#include "faio/detail/coroutine/lifetime/reference.hpp"
#include "faio/detail/coroutine/context/thread_context.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/execution_adapter/thread_binding.hpp"
#endif
#include <cstddef>
#include <limits>
#include <memory>

namespace faio::runtime::detail {
/** @brief 阻塞任务执行池。 */
class blocking_pool;
}  // namespace faio::runtime::detail

namespace faio::detail {
inline constexpr std::size_t no_worker_id = std::numeric_limits<std::size_t>::max();

template <class T>
inline constexpr unsigned char execution_binding_type_tag{};

/** @brief 执行线程的静态服务绑定。 */
template <class S = default_scheduler>
class execution_thread_binding;

/** @brief TLS 中统一保存的线程元数据；具体 binding 按调度器的实际类型保存它。 */
class execution_thread_binding_base
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    : public execution_host_binding
#endif
{
 public:
  execution_thread_binding_base(const execution_thread_binding_base&) = delete;
  execution_thread_binding_base& operator=(const execution_thread_binding_base&) = delete;

  template <class S = default_scheduler>
  basic_scheduler_ref<S> scheduler() const;

  template <class S>
  basic_scheduler_ref<S> try_scheduler() const noexcept;

  template <class S>
  bool matches(basic_scheduler_ref<S> target) const noexcept {
    return scheduler_type_ == &execution_binding_type_tag<S> && try_scheduler<S>() == target;
  }

  bool has_scheduler() const noexcept { return scheduler_type_ != nullptr; }

  task_lifetime_ref lifetime() const noexcept { return lifetime_; }

  std::size_t worker_id() const noexcept { return worker_id_; }

  int bound_cpu() const noexcept { return bound_cpu_; }

  ::faio::runtime::detail::blocking_pool* blocking() const noexcept { return blocking_; }

  template <class LocalState, class S>
  LocalState* local_state_for(basic_scheduler_ref<S> target) const noexcept {
    if (!matches(target) || local_type_ != &execution_binding_type_tag<LocalState>)
      return nullptr;
    return static_cast<LocalState*>(local_state_);
  }

 protected:
  ~execution_thread_binding_base() = default;

 private:
  template <class>
  friend class execution_thread_binding;

  template <class S, class LocalState>
  execution_thread_binding_base(basic_scheduler_ref<S> scheduler,
                                task_lifetime_ref lifetime,
                                LocalState& local_state,
                                std::size_t worker_id,
                                ::faio::runtime::detail::blocking_pool* blocking
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                                ,
                                external_work_host* external_host
#endif
                                ,
                                int bound_cpu) noexcept
      :
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
        execution_host_binding(external_host),
#endif
        scheduler_type_(scheduler ? &execution_binding_type_tag<S> : nullptr),
        lifetime_(lifetime),
        local_state_(std::addressof(local_state)),
        local_type_(&execution_binding_type_tag<LocalState>),
        worker_id_(worker_id),
        blocking_(blocking),
        bound_cpu_(bound_cpu) {
  }

 private:
  const void* scheduler_type_;
  task_lifetime_ref lifetime_;
  void* local_state_;
  const void* local_type_;
  std::size_t worker_id_;
  ::faio::runtime::detail::blocking_pool* blocking_{};
  int bound_cpu_{-1};
};

/** @brief 在稳定线程绑定中直接保存单指针静态调度引用，不擦除调度器指针类型。 */
template <class S>
class execution_thread_binding final : public execution_thread_binding_base {
 public:
  /** @brief 构造执行线程的静态服务绑定。 */
  template <class LocalState>
  execution_thread_binding(basic_scheduler_ref<S> scheduler,
                           task_lifetime_ref lifetime,
                           LocalState& local_state,
                           std::size_t worker_id,
                           ::faio::runtime::detail::blocking_pool* blocking = nullptr
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                           ,
                           external_work_host* external_host = nullptr
#endif
                           ,
                           int bound_cpu = -1) noexcept
      : execution_thread_binding_base(scheduler,
                                      lifetime,
                                      local_state,
                                      worker_id,
                                      blocking
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                                      ,
                                      external_host
#endif
                                      ,
                                      bound_cpu),
        scheduler_(scheduler) {
  }

  /** @brief 返回绑定中保存的静态调度器引用。 */
  basic_scheduler_ref<S> scheduler_reference() const noexcept { return scheduler_; }

 private:
  basic_scheduler_ref<S> scheduler_;
};

/** @brief 尝试取得当前绑定的静态调度器。 */
template <class S>
basic_scheduler_ref<S> execution_thread_binding_base::try_scheduler() const noexcept {
  if (scheduler_type_ != &execution_binding_type_tag<S>)
    return {};
  return static_cast<const execution_thread_binding<S>&>(*this).scheduler_reference();
}

/** @brief 取得绑定的调度器。 */
template <class S>
basic_scheduler_ref<S> execution_thread_binding_base::scheduler() const {
  if (scheduler_type_ && scheduler_type_ != &execution_binding_type_tag<S>)
    throw std::logic_error("线程绑定与静态调度器类型不匹配");
  return try_scheduler<S>();
}

/** @brief 外部提交的静态服务绑定。 */
template <class S = default_scheduler>
class submission_binding;

/** @brief 阻塞执行通道的提交服务元数据，不代表运行时工作线程。 */
class submission_binding_base
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    : public submission_host_binding
#endif
{
 public:
  submission_binding_base(const submission_binding_base&) = delete;
  submission_binding_base& operator=(const submission_binding_base&) = delete;

  template <class S>
  basic_scheduler_ref<S> scheduler() const;

  template <class S>
  basic_scheduler_ref<S> try_scheduler() const noexcept;

  template <class S>
  bool matches(basic_scheduler_ref<S> target) const noexcept {
    return scheduler_type_ == &execution_binding_type_tag<S> && try_scheduler<S>() == target;
  }

  bool has_scheduler() const noexcept { return scheduler_type_ != nullptr; }

  task_lifetime_ref lifetime;
  ::faio::runtime::detail::blocking_pool* blocking{};

 protected:
  ~submission_binding_base() = default;

 private:
  template <class>
  friend class submission_binding;

  template <class S>
  submission_binding_base(basic_scheduler_ref<S> scheduler,
                          task_lifetime_ref owner,
                          ::faio::runtime::detail::blocking_pool& pool
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                          ,
                          external_work_host* host
#endif
                          ) noexcept
      :
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
        submission_host_binding{host},
#endif
        lifetime(owner),
        blocking(&pool),
        scheduler_type_(scheduler ? &execution_binding_type_tag<S> : nullptr) {
  }

 private:
  const void* scheduler_type_;
};

template <class S>
class submission_binding final : public submission_binding_base {
 public:
  /** @brief 构造外部提交的静态服务绑定。 */
  submission_binding(basic_scheduler_ref<S> scheduler,
                     task_lifetime_ref lifetime,
                     ::faio::runtime::detail::blocking_pool& blocking
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                     ,
                     external_work_host* host
#endif
                     ) noexcept
      : submission_binding_base(scheduler,
                                lifetime,
                                blocking
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                                ,
                                host
#endif
                                ),
        scheduler_(scheduler) {
  }

  /** @brief 返回绑定中保存的静态调度器引用。 */
  basic_scheduler_ref<S> scheduler_reference() const noexcept { return scheduler_; }

 private:
  basic_scheduler_ref<S> scheduler_;
};

template <class S>
basic_scheduler_ref<S> submission_binding_base::try_scheduler() const noexcept {
  if (scheduler_type_ != &execution_binding_type_tag<S>)
    return {};
  return static_cast<const submission_binding<S>&>(*this).scheduler_reference();
}

template <class S>
basic_scheduler_ref<S> submission_binding_base::scheduler() const {
  if (scheduler_type_ && scheduler_type_ != &execution_binding_type_tag<S>)
    throw std::logic_error("线程绑定与静态调度器类型不匹配");
  return try_scheduler<S>();
}

template <class S>
class scoped_submission_binding {
 public:
  /** @brief 保存原提交绑定，并安装本作用域的服务绑定。 */
  scoped_submission_binding(basic_scheduler_ref<S> scheduler,
                            task_lifetime_ref lifetime,
                            ::faio::runtime::detail::blocking_pool& blocking
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                            ,
                            external_work_host* host
#endif
                            ) noexcept
      : binding_(scheduler,
                 lifetime,
                 blocking
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                 ,
                 host
#endif
                 ),
        previous_(current_thread_context().services.submission) {
    current_thread_context().services.submission = &binding_;
  }

  /** @brief 禁止复制提交服务守卫。 */
  scoped_submission_binding(const scoped_submission_binding&) = delete;
  /** @brief 禁止复制赋值。 */
  scoped_submission_binding& operator=(const scoped_submission_binding&) = delete;

  /** @brief 退出作用域时恢复原提交绑定。 */
  ~scoped_submission_binding() {
    if (auto* context = try_current_thread_context())
      context->services.submission = previous_;
  }

 private:
  submission_binding<S> binding_;
  const submission_binding_base* previous_;
};

class execution_thread_guard {
 public:
  /** @brief 保存原执行绑定，并安装指定线程绑定。 */
  explicit execution_thread_guard(const execution_thread_binding_base& binding) noexcept
      : previous_(current_thread_context().services.execution) {
    current_thread_context().services.execution = &binding;
  }

  /** @brief 禁止复制执行绑定守卫。 */
  execution_thread_guard(const execution_thread_guard&) = delete;
  /** @brief 禁止复制赋值。 */
  execution_thread_guard& operator=(const execution_thread_guard&) = delete;

  /** @brief 退出作用域时恢复原执行绑定。 */
  ~execution_thread_guard() {
    if (auto* context = try_current_thread_context())
      context->services.execution = previous_;
  }

 private:
  const execution_thread_binding_base* previous_;
};

inline const execution_thread_binding_base* current_execution_binding() noexcept {
  const auto* context = try_current_thread_context();
  return context ? context->services.execution : nullptr;
}

inline bool has_current_scheduler() noexcept {
  const auto* context = try_current_thread_context();
  if (!context)
    return false;
  const auto& services = context->services;
  return services.execution    ? services.execution->has_scheduler()
         : services.submission ? services.submission->has_scheduler()
                               : false;
}

template <class S>
bool current_scheduler_matches(basic_scheduler_ref<S> scheduler) noexcept {
  const auto* context = try_current_thread_context();
  if (!context)
    return false;
  const auto& services = context->services;
  return services.execution    ? services.execution->matches(scheduler)
         : services.submission ? services.submission->matches(scheduler)
                               : false;
}

inline scheduler_ref current_scheduler() noexcept {
  const auto* context = try_current_thread_context();
  if (!context)
    return {};
  const auto& services = context->services;
  return services.execution    ? services.execution->try_scheduler<default_scheduler>()
         : services.submission ? services.submission->try_scheduler<default_scheduler>()
                               : scheduler_ref{};
}

template <class S>
basic_scheduler_ref<S> current_scheduler_for() {
  const auto* context = try_current_thread_context();
  if (!context)
    return {};
  const auto& services = context->services;
  return services.execution    ? services.execution->scheduler<S>()
         : services.submission ? services.submission->scheduler<S>()
                               : basic_scheduler_ref<S>{};
}

template <class S>
basic_scheduler_ref<S> require_current_scheduler_for() {
  const auto scheduler = current_scheduler_for<S>();
  if (!scheduler)
    throw std::logic_error("任务需要有效的线程调度绑定");
  return scheduler;
}

inline task_lifetime_ref current_task_lifetime() noexcept {
  const auto* context = try_current_thread_context();
  if (!context)
    return {};
  const auto& services = context->services;
  return services.execution    ? services.execution->lifetime()
         : services.submission ? services.submission->lifetime
                               : task_lifetime_ref{};
}

inline std::size_t current_worker_id() noexcept {
  const auto* execution = current_execution_binding();
  return execution ? execution->worker_id() : no_worker_id;
}

inline int current_bound_cpu() noexcept {
  const auto* execution = current_execution_binding();
  return execution ? execution->bound_cpu() : -1;
}

inline bool on_runtime_worker() noexcept {
  return current_execution_binding() != nullptr;
}

inline ::faio::runtime::detail::blocking_pool* current_blocking_pool() noexcept {
  const auto* context = try_current_thread_context();
  if (!context)
    return nullptr;
  const auto& services = context->services;
  return services.execution    ? services.execution->blocking()
         : services.submission ? services.submission->blocking
                               : nullptr;
}

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
inline external_work_host* current_external_host() noexcept {
  const auto* context = try_current_thread_context();
  if (!context)
    return nullptr;
  const auto& services = context->services;
  return execution_host_for(services.execution, services.submission);
}
#endif
}  // namespace faio::detail

#endif
