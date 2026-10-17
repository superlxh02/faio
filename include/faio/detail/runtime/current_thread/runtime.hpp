/**
 * @file faio/detail/runtime/current_thread/runtime.hpp
 * @brief 运行时调度、线程协调与定时驱动：运行时驱动器。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#ifndef FAIO_DETAIL_RUNTIME_CURRENT_THREAD_RUNTIME_HPP
#define FAIO_DETAIL_RUNTIME_CURRENT_THREAD_RUNTIME_HPP

#include "faio/detail/coroutine/frame_allocator.hpp"
#include "faio/detail/coroutine/task_context.hpp"
#include "faio/detail/runtime/common/blocking_pool.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/common/io_services.hpp"
#include "faio/detail/runtime/common/root_task_counter.hpp"
#include "faio/detail/runtime/common/external_work.hpp"
#include "faio/detail/runtime/current_thread/scheduler.hpp"
#include <concepts>
#include <memory>
#include <mutex>
#include <utility>

namespace faio::runtime::detail {
/// @brief 异步任务由当前驱动线程执行；互斥锁保证多个调用者不会同时驱动运行时。
/// 就绪队列、定时器和在途 I/O 在相邻两次 block_on 之间保持有效。
class current_thread_runtime {
 public:
  explicit current_thread_runtime(const runtime_config& config, runtime_context* context = nullptr)
      : config_(config),
        io_services_(make_io_services(config)),
        roots_{scheduler_, {}},
        blocking_(config._max_blocking_threads,
                  config._blocking_keep_alive,
                  config._blocking_queue_limit),
        engine_(std::make_unique<io_engine>(config_, false, io_services_)) {
    /// @brief 先构造可能失败的资源，再接受借用 block_on 调用者栈的根任务，
    /// 避免初始化失败后留下无法排空的根任务。
    scheduler_.set_waker(engine_.get());
    external_.set_context(context);
    external_.set_waker(&scheduler_, +[](void* state, std::size_t) noexcept {
      static_cast<current_thread_scheduler*>(state)->notify_completion();
    });
    io_services_.placement_service->register_domain(0, engine_->context().domain());
  }

  current_thread_runtime(const current_thread_runtime&) = delete;

  current_thread_runtime& operator=(const current_thread_runtime&) = delete;

  ~current_thread_runtime() { stop(io::shutdown_policy::drain); }

  scheduler_ref scheduler() noexcept { return scheduler_ref{scheduler_}; }

  task_lifetime_ref lifetime() noexcept { return task_lifetime_ref{roots_}; }

  /// @brief 取得文件或通用阻塞执行服务引用，具体服务归属由上下文决定。
  blocking_pool& blocking() noexcept { return blocking_; }

  ::faio::detail::external_work_host& external_host() noexcept { return external_; }

  /// @brief 返回实际已初始化后端提供的能力快照。
  io::io_capabilities capabilities() const noexcept { return engine_->capabilities(); }

  void drive_until(const ::faio::detail::task_tracker& tracker) {
    drive([&] { return tracker.pending.load(std::memory_order_acquire) == 0; });
  }

  template <std::predicate Done>
  void drive_until(Done done) { drive(std::move(done)); }

  void stop(io::shutdown_policy policy = io::shutdown_policy::cancel_all) {
    if (stopped_)
      return;
    external_.begin_quiescing();
    external_.begin_draining();
    if (policy == io::shutdown_policy::cancel_all)
      external_.request_stop();
    if (policy == io::shutdown_policy::cancel_all)
      engine_->begin_shutdown(policy);
    drive([&] { return roots_.counter.count() == 0 && external_.quiescent(); });
    engine_->begin_shutdown(policy);
    /// @brief 最后一个 root 退出不代表析构 close/清理 publisher 已完成。
    drive([&] { return engine_->context().domain()->quiescent(); });
    /// @brief 等所有结果发布者退出后，再关闭调度器和持久化 I/O 驱动器。
    blocking_.close();
    close_io_services(io_services_);
    scheduler_.close();
    external_.mark_stopped();
    stopped_ = true;
  }

 private:
  struct lifetime_state {
    current_thread_scheduler& scheduler;  ///< 捕获的目标调度器，跨线程完成时将续体投递回所属运行时。
    root_task_counter counter;  ///< 运行时根任务计数器，关闭时必须等待计数归零。

    /// @brief 登记一个活动根任务，关闭流程须等待配对完成。
    void register_task() noexcept { counter.register_task(); }

    /// @brief 归还一个已登记根任务计数，并在归零时通知排空方。
    void finish_task() noexcept {
      counter.finish_task();
      scheduler.notify_completion();
    }
  };

  /// @brief 在给定预算内推进后端、截止时间与完成发布。
  template <std::predicate Done>
  void drive(Done done) {
    std::unique_lock drive_lock(drive_mutex_);
    if (done())
      return;
    io_engine::binding io_binding{*engine_};
    ::faio::detail::execution_thread_binding binding{
        scheduler_ref{scheduler_}, lifetime(), scheduler_, 0, &blocking_, &external_};
    ::faio::detail::execution_thread_guard thread_guard{binding};
    /// @brief 驱动栈全程保留缓存；退出清空并还原外层。
    ::faio::detail::coroutine_frame_cache_scope
        frame_cache;

    /// @brief 连续执行短小的 block_on 时，也要推进 I/O 和定时器事件。
    drive_io();
    while (!done()) {
      if (--io_remaining_ == 0
          || std::chrono::steady_clock::now() - last_io_drive_ >= config_._max_io_delay) {
        /// @brief 实际推进后端事件与定时器，并中断连续快速槽执行链以重新建立协作预算。
        drive_io();
        io_remaining_ = config_._io_interval;
      }
      const bool check_remote = --remote_remaining_ == 0;
      if (check_remote)
        remote_remaining_ = config_._global_queue_interval;
      check_remote_pending_ = check_remote_pending_ || check_remote;
      if (prefer_external_ && external_.has_ready(0)) {
        /// @brief 通过宿主分派守卫执行外部节点，节点回调可同步销毁其所属操作。
        execute_external();
        /// @brief 本次已给外部任务执行机会，下轮优先尝试协程任务以维持两类工作的公平性。
        prefer_external_ = false;
        continue;
      }
      const auto poll_remote = std::exchange(check_remote_pending_, false);
      if (auto task = scheduler_.next(poll_remote)) {
        /// @brief IO/让出后重新恢复拥有新的共享额度。
        ::faio::detail::cooperative_poll_scope poll_budget;
        /// @brief 先安装干净任务上下文；真正用户 task 的等待器在进入其代码前恢复自己的属性。
        ::faio::detail::scoped_task_context context{::faio::detail::task_context{}};
        task->resume();
        /// @brief 本次已执行协程任务，下轮允许优先消费外部任务图节点。
        prefer_external_ = true;
        continue;
      }
      if (external_.has_ready(0)) {
        /// @brief 通过宿主分派守卫执行外部节点，节点回调可同步销毁其所属操作。
        execute_external();
        /// @brief 本次已给外部任务执行机会，下轮优先尝试协程任务以维持两类工作的公平性。
        prefer_external_ = false;
        continue;
      }
      if (drive_io() || scheduler_.has_ready() || external_.has_ready(0))
        continue;
      if (!scheduler_.prepare_sleep())
        continue;
      /// @brief 完成通知可能唤醒线程，而最后的发布者仍使 done()
      /// 仍为 false；发布尾部只通知宿主版本变化，须先排空该尾部再最终检查完成条件，避免进入无后续内核唤醒的等待。
      if (external_.wait_for_wake_publisher()) {
        scheduler_.finish_sleep();
        continue;
      }
      if (!done() && !external_.has_ready(0))
        engine_->wait_and_drive(scheduler_, engine_->next_deadline());
      scheduler_.finish_sleep();
    }
    /// @brief 即使目标任务组已经完成，也要提交后台根任务发起的 I/O 操作。
    drive_io();
  }

  bool drive_io() {
    last_io_drive_ = std::chrono::steady_clock::now();
    return engine_->drive(scheduler_);
  }

  /// @brief 通过独立分派守卫执行外部节点，回调可同步销毁节点所属对象。
  void execute_external() noexcept {
    auto dispatch = external_.try_pop_external(0);
    /// @brief 其他执行路径已经取空目标队列时直接返回，不凭提示假定一定存在节点。
    if (!dispatch)
      return;
    /// @brief 为外部节点执行建立新的协作轮询预算，并在退出时恢复外层预算。
    ::faio::detail::cooperative_poll_scope budget;
    ::faio::detail::scoped_task_context context{{
        .stop_token = dispatch.root() ? dispatch.root()->stop_token() : std::stop_token{},
        .external_scope = dispatch.scope()}};
    /// @brief 借用当前已独占领取的外部节点，实际存活责任由分派守卫维持。
    auto* node = dispatch.node();
    /// @brief 执行前复制静态回调，回调内节点所属操作可能同步自销毁。
    auto execute = node->execute;
    /// @brief receiver 可以销毁 node；此后只访问独立 dispatch guard。
    execute(node);
  }

  const runtime_config config_;  ///< 校验后的运行时配置，服务与调度预算据此初始化。
  io::engine_config io_services_;  ///< 运行时共享 IO 服务配置，传给各工作线程的域。
  current_thread_scheduler scheduler_;  ///< 捕获的目标调度器，跨线程完成时将续体投递回所属运行时。
  ::faio::detail::external_work_host external_;  ///< 运行时拥有的外部任务宿主，协调接纳、分派、取消与排空。
  lifetime_state roots_;  ///< 当前仍由宿主管理的根任务数量。
  blocking_pool blocking_;  ///< 借用运行时通用阻塞任务池，避免在协程工作线程执行同步阻塞调用。
  std::unique_ptr<io_engine> engine_;  ///< 所属线程的 IO 引擎，统一驱动后端事件及超时。
  std::mutex drive_mutex_;  ///< 阻止同一当前线程运行时被多个同步驱动入口并发推进。
  bool stopped_{};  ///< 停止状态的跨线程发布标志，后续接纳和查询据此拒绝新工作。
  bool prefer_external_{true};  ///< 下一轮是否优先领取外部任务，使外部与协程任务交替获得机会。
  bool check_remote_pending_{};  ///< 是否欠一次远程提交队列的公平性检查。
  std::chrono::steady_clock::time_point last_io_drive_{std::chrono::steady_clock::now()};  ///< 最近一次计时 IO 驱动的保守起点，用于限制驱动延迟。
  std::uint32_t io_remaining_{config_._io_interval};  ///< 距离下一次周期 IO 驱动的剩余调度轮数。
  std::uint32_t remote_remaining_{config_._global_queue_interval};  ///< 距离下一次远程队列公平性检查的剩余轮数。
};
}
#endif
