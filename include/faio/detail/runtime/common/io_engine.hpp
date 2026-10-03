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
    io::io_engine::binding io_binding_;
    io_engine* previous_;
    timer::Timer* timer_;
  };

  template <ready_sink S>
  bool drive(S& sink) {
    auto result = engine_.driver().drive(budget_);
    return timer_.poll(sink) > 0 || result.progressed || result.more_work;
  }

  template <ready_sink S>
  void wait_and_drive(S& sink) {
    wait_and_drive(sink, next_deadline());
  }

  template <ready_sink S>
  void wait_and_drive(S& sink, std::optional<time_t> deadline) {
    (void)engine_.driver().wait_and_drive(
        deadline ? std::optional<int>{static_cast<int>(std::min<time_t>(*deadline, INT_MAX))}
                 : std::nullopt,
        budget_);
    (void)timer_.poll(sink);
  }

  std::optional<time_t> next_deadline() noexcept {
    const auto timer_deadline = timer_.next_deadline_ms();
    auto deadline = engine_.context().domain()->next_deadline();
    if (timer_deadline != std::numeric_limits<std::size_t>::max()
        && (!deadline || timer_deadline < static_cast<std::size_t>(*deadline)))
      deadline = static_cast<int>(std::min<std::size_t>(timer_deadline, INT_MAX));
    return deadline ? std::optional<time_t>{*deadline} : std::nullopt;
  }

  void wake_up() noexcept { engine_.driver().wake(); }

  io::io_context context() const noexcept { return engine_.context(); }

  io::io_capabilities capabilities() const noexcept { return engine_.capabilities(); }

  void begin_shutdown(io::shutdown_policy policy = io::shutdown_policy::cancel_all) noexcept {
    engine_.begin_shutdown(policy);
  }

 private:
  io::io_engine engine_;
  timer::Timer timer_;
  io::drive_budget budget_;
  bool bound_{};
  io_engine* previous_{};
  io::detail::io_domain* previous_domain_{};
};
}  // namespace faio::runtime::detail
