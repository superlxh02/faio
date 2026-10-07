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
// 异步任务由当前驱动线程执行；互斥锁保证多个调用者不会同时驱动运行时。
// 就绪队列、定时器和在途 I/O 在相邻两次 block_on 之间保持有效。
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
    // 先构造可能失败的资源，再接受借用 block_on 调用者栈的根任务，
    // 避免初始化失败后留下无法排空的根任务。
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

  blocking_pool& blocking() noexcept { return blocking_; }

  ::faio::detail::external_work_host& external_host() noexcept { return external_; }

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
    // 最后一个 root 退出不代表析构 close/清理 publisher 已完成。
    drive([&] { return engine_->context().domain()->quiescent(); });
    // 等所有结果发布者退出后，再关闭调度器和持久化 I/O 驱动器。
    blocking_.close();
    close_io_services(io_services_);
    scheduler_.close();
    external_.mark_stopped();
    stopped_ = true;
  }

 private:
  struct lifetime_state {
    current_thread_scheduler& scheduler;
    root_task_counter counter;

    void register_task() noexcept { counter.register_task(); }

    void finish_task() noexcept {
      counter.finish_task();
      scheduler.notify_completion();
    }
  };

  template <std::predicate Done>
  void drive(Done done) {
    std::unique_lock drive_lock(drive_mutex_);
    if (done())
      return;
    io_engine::binding io_binding{*engine_};
    ::faio::detail::execution_thread_binding binding{
        scheduler_ref{scheduler_}, lifetime(), scheduler_, 0, &blocking_, &external_};
    ::faio::detail::execution_thread_guard thread_guard{binding};
    ::faio::detail::coroutine_frame_cache_scope
        frame_cache;  // 驱动栈全程保留缓存；退出清空并还原外层。

    // 连续执行短小的 block_on 时，也要推进 I/O 和定时器事件。
    drive_io();
    while (!done()) {
      if (--io_remaining_ == 0
          || std::chrono::steady_clock::now() - last_io_drive_ >= config_._max_io_delay) {
        drive_io();
        io_remaining_ = config_._io_interval;
      }
      const bool check_remote = --remote_remaining_ == 0;
      if (check_remote)
        remote_remaining_ = config_._global_queue_interval;
      check_remote_pending_ = check_remote_pending_ || check_remote;
      if (prefer_external_ && external_.has_ready(0)) {
        execute_external();
        prefer_external_ = false;
        continue;
      }
      const auto poll_remote = std::exchange(check_remote_pending_, false);
      if (auto task = scheduler_.next(poll_remote)) {
        ::faio::detail::cooperative_poll_scope poll_budget;  // IO/让出后重新恢复拥有新的共享额度。
        ::faio::detail::scoped_task_context context{::faio::detail::task_context{}};
        task->resume();
        prefer_external_ = true;
        continue;
      }
      if (external_.has_ready(0)) {
        execute_external();
        prefer_external_ = false;
        continue;
      }
      if (drive_io() || scheduler_.has_ready() || external_.has_ready(0))
        continue;
      if (!scheduler_.prepare_sleep())
        continue;
      // Completion can wake us while its final publisher still keeps done()
      // false. Its tail only signals the host revision, so settle that tail
      // before the final predicate check rather than sleeping in the kernel.
      if (external_.wait_for_wake_publisher()) {
        scheduler_.finish_sleep();
        continue;
      }
      if (!done() && !external_.has_ready(0))
        engine_->wait_and_drive(scheduler_, engine_->next_deadline());
      scheduler_.finish_sleep();
    }
    // 即使目标任务组已经完成，也要提交后台根任务发起的 I/O 操作。
    drive_io();
  }

  bool drive_io() {
    last_io_drive_ = std::chrono::steady_clock::now();
    return engine_->drive(scheduler_);
  }

  void execute_external() noexcept {
    auto dispatch = external_.try_pop_external(0);
    if (!dispatch)
      return;
    ::faio::detail::cooperative_poll_scope budget;
    ::faio::detail::scoped_task_context context{{
        .stop_token = dispatch.root() ? dispatch.root()->stop_token() : std::stop_token{},
        .external_scope = dispatch.scope()}};
    auto* node = dispatch.node();
    auto execute = node->execute;
    execute(node);  // receiver 可以销毁 node；此后只访问独立 dispatch guard。
  }

  const runtime_config config_;
  io::engine_config io_services_;
  current_thread_scheduler scheduler_;
  ::faio::detail::external_work_host external_;
  lifetime_state roots_;
  blocking_pool blocking_;
  std::unique_ptr<io_engine> engine_;
  std::mutex drive_mutex_;
  bool stopped_{};
  bool prefer_external_{true};
  bool check_remote_pending_{};
  std::chrono::steady_clock::time_point last_io_drive_{std::chrono::steady_clock::now()};
  std::uint32_t io_remaining_{config_._io_interval};
  std::uint32_t remote_remaining_{config_._global_queue_interval};
};
}  // namespace faio::runtime::detail
#endif
