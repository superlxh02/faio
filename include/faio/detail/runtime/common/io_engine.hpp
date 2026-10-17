/**
 * @file faio/detail/runtime/common/io_engine.hpp
 * @brief 运行时调度、线程协调与定时驱动：运行时 IO 驱动桥接。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#pragma once
#include "faio/detail/coroutine/scheduler.hpp"
#include "faio/detail/io/engine.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/timer/timer.hpp"
#include <limits>
#include <optional>

namespace faio::runtime::detail {
class io_engine;
inline thread_local io_engine* current_io_engine{};

/** @brief runtime 的组合适配器；IO 核心与 timer/scheduler 保持单向依赖。 */
class io_engine {
 public:
  explicit io_engine(const runtime_config& config,
                     bool bind_thread = true,
                     io::engine_config services = {})
      : engine_(std::move(services)),
        timer_(bind_thread),
        budget_{std::clamp<std::size_t>(config._num_events, 1, 256), 256},
        bound_(bind_thread),
        previous_(current_io_engine),
        previous_domain_(io::detail::current_domain) {
    if (bind_thread) {
      current_io_engine = this;
      io::detail::current_domain = engine_.context().domain().get();
    }
  }

  ~io_engine() {
    engine_.shutdown();
    if (bound_) {
      current_io_engine = previous_;
      io::detail::current_domain = previous_domain_;
    }
  }

  class binding {
   public:
    explicit binding(io_engine& engine) noexcept
        : io_binding_(engine.engine_), previous_(current_io_engine), timer_(timer::current_timer) {
      current_io_engine = &engine;
      timer::current_timer = &engine.timer_;
    }

    ~binding() {
      current_io_engine = previous_;
      timer::current_timer = timer_;
    }

   private:
    io::io_engine::binding io_binding_;  ///< 安装当前线程 IO 域并在析构时恢复原绑定的守卫。
    io_engine* previous_;  ///< 进入当前作用域前保存的线程绑定或任务上下文，析构时恢复。
    timer::Timer* timer_;  ///< 关联的时间轮管理器，登记和取消须遵循所属线程协议。
  };

  /// @brief 在给定预算内推进后端、截止时间与完成发布。
  template <ready_sink S>
  bool drive(S& sink) {
    auto result = engine_.driver().drive(budget_);
    return timer_.poll(sink) > 0 || result.progressed || result.more_work;
  }

  /// @brief 等待可唤醒的底层事件后推进本域 IO 与定时任务。
  template <ready_sink S>
  void wait_and_drive(S& sink) {
    wait_and_drive(sink, next_deadline());
  }

  /// @brief 等待可唤醒的底层事件后推进本域 IO 与定时任务。
  template <ready_sink S>
  void wait_and_drive(S& sink, std::optional<time_t> deadline) {
    (void)engine_.driver().wait_and_drive(
        deadline ? std::optional<int>{static_cast<int>(std::min<time_t>(*deadline, INT_MAX))}
                 : std::nullopt,
        budget_);
    (void)timer_.poll(sink);
  }

  /// @brief 计算到最近截止时间的非负毫秒数，作为底层等待上限。
  std::optional<time_t> next_deadline() noexcept {
    const auto timer_deadline = timer_.next_deadline_ms();
    auto deadline = engine_.context().domain()->next_deadline();
    if (timer_deadline != std::numeric_limits<std::size_t>::max()
        && (!deadline || timer_deadline < static_cast<std::size_t>(*deadline)))
      deadline = static_cast<int>(std::min<std::size_t>(timer_deadline, INT_MAX));
    return deadline ? std::optional<time_t>{*deadline} : std::nullopt;
  }

  /// @brief 发送独立控制通知，打断底层等待以处理已经发布的工作。
  void wake_up() noexcept { engine_.driver().wake(); }

  /// @brief 取得当前对象关联的 IO 上下文租约。
  io::io_context context() const noexcept { return engine_.context(); }

  /// @brief 返回实际已初始化后端提供的能力快照。
  io::io_capabilities capabilities() const noexcept { return engine_.capabilities(); }

  /// @brief 停止接受新业务，按停止策略取消或保留已接受请求并启动排空。
  void begin_shutdown(io::shutdown_policy policy = io::shutdown_policy::cancel_all) noexcept {
    engine_.begin_shutdown(policy);
  }

 private:
  io::io_engine engine_;  ///< 所属线程的 IO 引擎，统一驱动后端事件及超时。
  timer::Timer timer_;  ///< 关联的时间轮管理器，登记和取消须遵循所属线程协议。
  io::drive_budget budget_;  ///< 本次事件循环的后端事件与完成发布预算。
  bool bound_{};  ///< 当前线程是否已经安装本运行时绑定。
  io_engine* previous_{};  ///< 进入当前作用域前保存的线程绑定或任务上下文，析构时恢复。
  io::detail::io_domain* previous_domain_{};  ///< 进入绑定前的 IO 域，退出时恢复以支持嵌套调用。
};
}
