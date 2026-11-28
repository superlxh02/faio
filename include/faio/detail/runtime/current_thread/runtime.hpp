/**
 * @file faio/detail/runtime/current_thread/runtime.hpp
 * @brief 运行时驱动器。
 */
#ifndef FAIO_DETAIL_RUNTIME_CURRENT_THREAD_RUNTIME_HPP
#define FAIO_DETAIL_RUNTIME_CURRENT_THREAD_RUNTIME_HPP

#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/coroutine/lifetime/tracker.hpp"
#include "faio/detail/coroutine/memory/frame_allocator.hpp"
#include "faio/detail/coroutine/memory/frame_cache_binding.hpp"
#include "faio/detail/coroutine/memory/large_frame_pool_binding.hpp"
#include "faio/detail/runtime/common/blocking_pool.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/common/worker_affinity.hpp"
#include "faio/detail/runtime/common/io_services.hpp"
#include "faio/detail/runtime/common/io_engine.hpp"
#include "faio/detail/runtime/common/root_task_counter.hpp"
#include "faio/detail/runtime/common/completion_service.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/driver.hpp"
#endif
#include "faio/detail/runtime/current_thread/scheduler.hpp"
#include <concepts>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace faio::runtime::detail {
/** @brief 运行时服务与调度上下文。 */
class runtime_context;

/**
 * @brief 异步任务由当前驱动线程执行；互斥锁保证多个调用者不会同时驱动运行时。
 * 就绪队列、定时器和在途 I/O 在相邻两次 block_on 之间保持有效。
 */
class current_thread_runtime {
 public:
  /** @brief 构造单线程运行时。 */
  explicit current_thread_runtime(const runtime_config& config, runtime_context* context = nullptr)
      : config_(config),
        large_frames_(config._large_frame_pool_limit_bytes),
        io_services_(make_io_services(config)),
        roots_{completion_},
        blocking_(config._max_blocking_threads,
                  config._blocking_keep_alive,
                  config._blocking_queue_limit,
                  config._blocking_pool_policy),
        engine_(std::make_unique<io_engine>(config_, false, io_services_)),
        io_waker_(*engine_->context().domain()),
        local_(scheduler_,
               0,
               config_._global_queue_interval,
               basic_worker_waker_ref{io_waker_},
               config_._no_stealing_queue_capacity) {
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    completion_.set_context(context);
#endif
#if !defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) || !FAIO_EXPERIMENTAL_HAS_EXECUTION
    (void)context;
#endif
    completion_.set_waker(basic_target_waker_ref{scheduler_});
    io_services_.placement_service->register_domain(0, engine_->context().domain());
  }

  /** @brief 禁止复制构造单线程运行时。 */
  current_thread_runtime(const current_thread_runtime&) = delete;

  /** @brief 禁止复制赋值。 */
  current_thread_runtime& operator=(const current_thread_runtime&) = delete;

  /** @brief 销毁单线程运行时。 */
  ~current_thread_runtime() { stop(io::shutdown_policy::DRAIN); }

  /** @brief 取得绑定的调度器。 */
  scheduler_ref scheduler() noexcept { return scheduler_ref{scheduler_}; }

  /** @brief 取得任务生命周期服务。 */
  task_lifetime_ref lifetime() noexcept { return task_lifetime_ref{roots_}; }

  /** @brief 取得文件或通用阻塞执行服务引用，具体服务归属由上下文决定。 */
  blocking_pool& blocking() noexcept { return blocking_; }

  /** @brief 取得任务完成发布服务。 */
  auto& completion_service() noexcept { return completion_; }

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  /** @brief 取得关联的外部执行宿主。 */
  ::faio::detail::external_work_host& external_host() noexcept { return completion_; }
#endif

  /** @brief 返回实际已初始化后端提供的能力快照。 */
  io::io_capabilities capabilities() const noexcept { return engine_->capabilities(); }

  /** @brief 驱动运行时直到指定条件满足。 */
  void drive_until(const ::faio::detail::task_tracker& tracker) {
    drive([&] { return tracker.pending.load(std::memory_order_acquire) == 0; });
  }

  /** @brief 驱动运行时直到指定条件满足。 */
  template <std::predicate Done>
  void drive_until(Done done) {
    drive(std::move(done));
  }

  /** @brief 停止接纳并排空运行时工作。 */
  void stop(io::shutdown_policy policy = io::shutdown_policy::CANCEL_ALL) {
    stop_impl(policy, false);
  }

 private:
  friend class runtime_context;

  /**  @brief 调用方在改变接纳状态前已取得亲和性守卫，停止过程中不再重复执行可能失败的绑定。 */
  void stop_prebound(io::shutdown_policy policy) { stop_impl(policy, true); }

  /** @brief 执行运行时停止流程。 */
  void stop_impl(io::shutdown_policy policy, bool affinity_prepared) {
    std::lock_guard stop_lock(stop_mutex_);
    if (stopped_)
      return;
    ::faio::detail::scoped_thread_context cleanup_context;
    std::optional<worker_affinity_scope> affinity;
    if (!affinity_prepared && !config_._worker_cpus.empty())
      affinity.emplace(config_._worker_cpus);
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    completion_.begin_quiescing();
    completion_.begin_draining();
    if (policy == io::shutdown_policy::CANCEL_ALL)
      completion_.request_stop();
#endif
    if (policy == io::shutdown_policy::CANCEL_ALL)
      engine_->begin_shutdown(policy);
    std::unique_lock drive_lock(drive_mutex_);
    drive_locked([&] { return roots_.counter.count() == 0 && completion_.quiescent(); }, true);
    engine_->begin_shutdown(policy);
    drive_locked([&] { return engine_->context().domain()->quiescent(); }, true);
    blocking_.close();
    close_io_services(io_services_);
    scheduler_.close();
    completion_.mark_stopped();
    frame_cache_.clear();
    large_frames_.retire();
    stopped_ = true;
  }

  /**  @brief runtime_context 在接纳借用栈上数据的根任务前已持有亲和性守卫，此处不重复执行可能失败的绑定。 */
  void drive_until_prebound(const ::faio::detail::task_tracker& tracker) {
    drive([&] { return tracker.pending.load(std::memory_order_acquire) == 0; }, true);
  }

  /** @brief 驱动退出时撤销未完成的休眠握手，并归还远程通知预登记的搜索名额。 */
  class driver_state_scope {
   public:
    /** @brief 构造 I/O 驱动状态作用域。 */
    explicit driver_state_scope(local_scheduler& scheduler) noexcept : scheduler_(scheduler) {}

    /** @brief 禁止复制构造 I/O 驱动状态作用域。 */
    driver_state_scope(const driver_state_scope&) = delete;
    /** @brief 禁止复制赋值。 */
    driver_state_scope& operator=(const driver_state_scope&) = delete;

    /** @brief 退出 I/O 驱动作用域时撤销休眠登记，并整理调度状态。 */
    ~driver_state_scope() {
      if (sleeping_)
        (void)scheduler_.finish_sleep(true);
      scheduler_.before_execute();
    }

    /** @brief 登记工作线程即将休眠。 */
    bool prepare_sleep() {
      sleeping_ = scheduler_.prepare_sleep();
      return sleeping_;
    }

    /** @brief 结束休眠登记。 */
    bool finish_sleep(bool ready) {
      if (!scheduler_.finish_sleep(ready))
        return false;
      sleeping_ = false;
      return true;
    }

   private:
    local_scheduler& scheduler_;
    bool sleeping_{};
  };

  /**
   * @brief 同步恢复协程期间清空当前任务上下文，退出时恢复外层上下文。
   * @details 线程上下文在本作用域内持续有效。移动停止令牌和取消链所有权，
   *          避免在保存外层上下文时修改引用计数。
   */
  class resume_task_context {
   public:
    /** @brief 保存外层任务上下文，并安装空上下文。 */
    resume_task_context() noexcept
        : active_(::faio::detail::current_thread_context().task_ctx),
          previous_(std::exchange(active_, ::faio::detail::task_context{})) {}

    /** @brief 禁止复制构造任务上下文守卫。 */
    resume_task_context(const resume_task_context&) = delete;
    /** @brief 禁止复制赋值。 */
    resume_task_context& operator=(const resume_task_context&) = delete;

    /** @brief 在恢复协作预算前恢复外层任务上下文。 */
    ~resume_task_context() { active_ = std::move(previous_); }

   private:
    ::faio::detail::task_context& active_;
    ::faio::detail::task_context previous_;
  };

  /** @brief 在给定预算内推进后端、截止时间与完成发布。 */
  template <std::predicate Done>
  void drive(Done done, bool affinity_prepared = false) {
    std::unique_lock drive_lock(drive_mutex_);
    drive_locked(std::move(done), affinity_prepared);
  }

  /**
   * @brief 在已持有驱动锁的情况下运行任务，停止路径可直接调用。
   * @details 每轮读取一次时钟，驱动 I/O 时沿用此时间作为保守起点。
   */
  template <std::predicate Done>
  void drive_locked(Done done, bool affinity_prepared = false) {
    if (done())
      return;
    if (stopped_)
      throw std::logic_error("current_thread runtime 已停止");
    std::optional<worker_affinity_scope> affinity;
    if (!affinity_prepared && !config_._worker_cpus.empty())
      affinity.emplace(config_._worker_cpus);
    ::faio::detail::large_frame_pool_binding large_frames{
        config_._large_frame_pool_limit_bytes ? &large_frames_ : nullptr};
    io_engine::binding io_binding{*engine_};
    ::faio::detail::execution_thread_binding binding{
        scheduler_ref{scheduler_},
        lifetime(),
        local_.local_state(),
        0,
        &blocking_
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
        ,
        &completion_
#endif
        ,
        config_._worker_cpus.empty() ? -1 : config_._worker_cpus.front()};
    ::faio::detail::execution_thread_guard thread_guard{binding};
    ::faio::detail::coroutine_frame_cache_binding frame_cache_binding{frame_cache_};
    driver_state_scope driver_state{local_};

    drive_io_at(std::chrono::steady_clock::now());
    while (!done()) {
      const auto now = std::chrono::steady_clock::now();
      if (--io_remaining_ == 0 || now - last_io_drive_ >= config_._max_io_delay) {
        drive_io_at(now);
        io_remaining_ = config_._io_interval;
      }
      const bool check_remote = --remote_remaining_ == 0;
      if (check_remote)
        remote_remaining_ = config_._global_queue_interval;
      check_remote_pending_ = check_remote_pending_ || check_remote;
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
      if (execution_.preferred(completion_, 0)) {
        execution_.execute(completion_, local_, 0);
        continue;
      }
#endif
      const auto poll_remote = std::exchange(check_remote_pending_, false);
      const auto task = current_thread_owner_scheduler::next(local_, poll_remote);
      if (task) {
        local_.before_execute();
        auto poll_budget = local_.begin_execution();
        resume_task_context context;
        task->resume();
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
        execution_.native_progress();
#endif
        continue;
      }
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
      if (completion_.has_ready(0)) {
        execution_.execute(completion_, local_, 0);
        continue;
      }
#endif
      if (drive_io_at(now) || local_.has_ready_task() || additional_ready())
        continue;
      local_.interrupt_execution_chain();
      frame_cache_.flush();
      if (!driver_state.prepare_sleep())
        continue;
      while (true) {
        if (completion_.wait_for_wake_publisher()) {
          (void)driver_state.finish_sleep(true);
          break;
        }
        if (done() || local_.has_ready_task() || additional_ready()) {
          (void)driver_state.finish_sleep(true);
          break;
        }
        engine_->wait_and_drive(local_, engine_->next_deadline());
        if (driver_state.finish_sleep(done() || additional_ready()))
          break;
      }
    }
    local_.before_execute();
    drive_io();
  }

  /** @brief 记录本次驱动的时间起点，供下一轮 I/O 公平预算判断。 */
  bool drive_io_at(std::chrono::steady_clock::time_point started) {
    last_io_drive_ = started;
    return drive_io();
  }

  /** @brief 驱动一次 I/O 事件处理。 */
  bool drive_io() {
    local_.interrupt_execution_chain();
    const bool progressed = engine_->drive(local_);
    if (progressed)
      local_.flush_ready();
    return progressed;
  }

  /** @brief 检查额外的就绪来源。 */
  bool additional_ready() const noexcept {
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    return completion_.has_ready(0);
#else
    return false;
#endif
  }

  const runtime_config config_; /**< 校验后的运行时配置，服务与调度预算据此初始化。 */
  ::faio::detail::large_frame_pool
      large_frames_;              /**< 持久页池，关闭后活动映射仍可凭 extent 引用安全归还。 */
  io::engine_config io_services_; /**< 运行时共享 IO 服务配置，传给各工作线程的域。 */
  domain_scheduler scheduler_{1}; /**< 本运行时持有的共享调度域，接收跨线程完成时投递的续体。 */
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  ::faio::detail::external_work_host completion_;
  execution_driver execution_;
#else
  ::faio::detail::runtime_completion_service completion_;
#endif
  ::faio::detail::task_lifetime_service roots_; /**< 当前仍由宿主管理的根任务数量。 */
  blocking_pool
      blocking_; /**< 本运行时拥有的通用阻塞任务池，避免在协程工作线程执行同步阻塞调用。 */
  std::unique_ptr<io_engine> engine_; /**< 所属线程的 IO 引擎，统一驱动后端事件及超时。 */
  io_worker_waker io_waker_;          /**< 具体 I/O 域的唤醒端，生命周期覆盖本地注册。 */
  local_scheduler local_;             /**< 单个持久 worker 与线程池共用本地队列和休眠协议。 */
  std::mutex drive_mutex_;            /**< 阻止同一当前线程运行时被多个同步驱动入口并发推进。 */
  ::faio::detail::coroutine_frame_cache
      frame_cache_;       /**< 运行时持有的空闲存储，在多次驱动之间复用，由 drive_mutex_ 保护。 */
  std::mutex stop_mutex_; /**< 串行化停止的取消发布；等待 drive 锁前即可安全检查停止状态。 */
  bool stopped_{}; /**< 写入时同时持有 stop 和 drive 两把锁；drive 与 stop 路径分别持对应锁读取。 */
  bool check_remote_pending_{}; /**< 是否有一次远程提交队列的公平性检查尚未执行。 */
  std::chrono::steady_clock::time_point last_io_drive_{
      std::chrono::steady_clock::now()}; /**< 最近一次计时 IO 驱动的保守起点，用于限制驱动延迟。 */
  std::uint32_t io_remaining_{config_._io_interval}; /**< 距离下一次周期 IO 驱动的剩余调度轮数。 */
  std::uint32_t remote_remaining_{
      config_._global_queue_interval}; /**< 距离下一次远程队列公平性检查的剩余轮数。 */
};
}  // namespace faio::runtime::detail
#endif
