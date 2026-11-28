/**
 * @file faio/detail/runtime/common/io_engine.hpp
 * @brief 运行时 I/O 驱动桥接。
 */
#pragma once
#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/runtime/common/coroutine_integration.hpp"
#include "faio/detail/io/engine.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/timer/timer.hpp"
#include <limits>
#include <optional>

namespace faio::runtime::detail {
/** @brief I/O 提交与驱动引擎。 */
class io_engine;

/**  @brief 运行时的组合适配器；I/O 核心与定时器、调度器保持单向依赖。 */
class io_engine {
 public:
  /** @brief 构造 I/O 提交与驱动引擎。 */
  explicit io_engine(const runtime_config& config,
                     bool bind_thread = true,
                     io::engine_config services = {})
      : engine_(std::move(services)),
        timer_(bind_thread),
        budget_{std::clamp<std::size_t>(config._num_events, 1, 256), 256},
        bound_(bind_thread),
        previous_(::faio::detail::current_thread_context().io.engine),
        previous_domain_(::faio::detail::current_thread_context().io.domain) {
    if (bind_thread) {
      ::faio::detail::current_thread_context().io.engine = this;
      ::faio::detail::current_thread_context().io.domain = engine_.context().domain().get();
    }
  }

  /** @brief 销毁 I/O 提交与驱动引擎。 */
  ~io_engine() {
    ::faio::detail::scoped_thread_context cleanup_context;
    engine_.shutdown();
    if (auto* context = ::faio::detail::try_current_thread_context(); bound_ && context) {
      context->io.engine = previous_;
      context->io.domain = previous_domain_;
    }
  }

  /** @brief I/O 驱动的作用域绑定。 */
  class binding {
   public:
    /** @brief 安装本作用域的 I/O 引擎和定时器绑定。 */
    explicit binding(io_engine& engine) noexcept
        : io_binding_(engine.engine_),
          previous_(::faio::detail::current_thread_context().io.engine),
          timer_(::faio::detail::current_thread_context().io.timer) {
      ::faio::detail::current_thread_context().io.engine = &engine;
      ::faio::detail::current_thread_context().io.timer = &engine.timer_;
    }

    /** @brief 恢复原 I/O 引擎和定时器绑定。 */
    ~binding() {
      if (auto* context = ::faio::detail::try_current_thread_context()) {
        context->io.engine = previous_;
        context->io.timer = timer_;
      }
    }

   private:
    io::io_engine::binding io_binding_; /**< 安装当前线程 IO 域并在析构时恢复原绑定的守卫。 */
    io_engine* previous_;               /**< 进入当前作用域前绑定的 I/O 引擎，析构时恢复。 */
    timer::timer* timer_;               /**< 关联的时间轮管理器，登记和取消须遵循所属线程协议。 */
  };

  /** @brief 在给定预算内推进后端、截止时间与完成发布。 */
  template <ready_sink S>
  bool drive(S& sink) {
    auto result = engine_.driver().drive(budget_);
    return timer_.poll(sink) > 0 || result.progressed || result.more_work;
  }

  /** @brief 等待可唤醒的底层事件后推进本域 IO 与定时任务。 */
  template <ready_sink S>
  void wait_and_drive(S& sink) {
    wait_and_drive(sink, next_deadline());
  }

  /** @brief 等待可唤醒的底层事件后推进本域 IO 与定时任务。 */
  template <ready_sink S>
  void wait_and_drive(S& sink, std::optional<time_t> deadline) {
    (void)engine_.driver().wait_and_drive(
        deadline ? std::optional<int>{static_cast<int>(std::min<time_t>(*deadline, INT_MAX))}
                 : std::nullopt,
        budget_);
    (void)timer_.poll(sink);
  }

  /** @brief 计算到最近截止时间的非负毫秒数，作为底层等待上限。 */
  std::optional<time_t> next_deadline() noexcept {
    const auto timer_deadline = timer_.next_deadline_ms();
    auto deadline = engine_.context().domain()->next_deadline();
    if (timer_deadline != std::numeric_limits<std::size_t>::max()
        && (!deadline || timer_deadline < static_cast<std::size_t>(*deadline)))
      deadline = static_cast<int>(std::min<std::size_t>(timer_deadline, INT_MAX));
    return deadline ? std::optional<time_t>{*deadline} : std::nullopt;
  }

  /** @brief 发送独立控制通知，打断底层等待以处理已经发布的工作。 */
  void wake_up() noexcept { engine_.driver().wake(); }

  /** @brief 取得当前对象关联的 IO 上下文租约。 */
  io::io_context context() const noexcept { return engine_.context(); }

  /** @brief 返回实际已初始化后端提供的能力快照。 */
  io::io_capabilities capabilities() const noexcept { return engine_.capabilities(); }

  /** @brief 停止接受新业务，按停止策略取消或保留已接受请求并启动排空。 */
  void begin_shutdown(io::shutdown_policy policy = io::shutdown_policy::CANCEL_ALL) noexcept {
    engine_.begin_shutdown(policy);
  }

 private:
  io::io_engine engine_;    /**< 所属线程的 IO 引擎，统一驱动后端事件及超时。 */
  timer::timer timer_;      /**< 关联的时间轮管理器，登记和取消须遵循所属线程协议。 */
  io::drive_budget budget_; /**< 本次事件循环的后端事件与完成发布预算。 */
  bool bound_{};            /**< 当前线程是否已经安装本运行时绑定。 */
  io_engine* previous_{};   /**< 进入当前作用域前绑定的 I/O 引擎，析构时恢复。 */
  io::detail::io_domain* previous_domain_{}; /**< 进入绑定前的 IO 域，退出时恢复以支持嵌套调用。 */
};
}  // namespace faio::runtime::detail
