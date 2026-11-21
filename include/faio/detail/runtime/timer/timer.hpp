/**
 * @file faio/detail/runtime/timer/timer.hpp
 * @brief 时间轮管理与层级调整。
 */
#ifndef FAIO_DETAIL_RUNTIME_TIMER_TIMER_HPP
#define FAIO_DETAIL_RUNTIME_TIMER_TIMER_HPP

#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/runtime/common/constants.hpp"
#include "faio/detail/runtime/timer/task.hpp"
#include "faio/detail/runtime/timer/wheel.hpp"
#include "faio/detail/log/logger.hpp"
#include <atomic>
#include <chrono>
#include <variant>

namespace faio::runtime::detail::timer {

/**
 * @brief 构造可保存第 0 至 N-1 层时间轮的 std::variant 类型。
 * variant 的备选类型依次为 std::monostate、unique_ptr<TimerWheel<0>>、
 * unique_ptr<TimerWheel<1>>，直至 unique_ptr<TimerWheel<N-1>>。
 */

template <class Indices>
struct variant_wheel_type;

/** @brief 按层级索引生成时间轮类型的类型特征。 */
template <std::size_t... N>
struct variant_wheel_type<std::index_sequence<N...>> {
  using type = std::variant<std::monostate, std::unique_ptr<TimerWheel<N>>...>;
};

/** @brief 分层时间轮类型构造器。 */
template <std::size_t N>
struct VariantWheelBuilder {
  using Type = typename variant_wheel_type<
      std::make_index_sequence<N>>::type; /**< 仅形成类型，不为类型查询实例化构造及析构。 */
};

/**
 * @brief 对外提供时间轮操作的定时器管理器。
 * @details
 * - 每个 worker 线程拥有独立的 Timer，通过统一线程上下文借用。
 * - 内部用 variant 存放不同层级的根时间轮，支持动态升降级。
 * - _start 始终对应根轮第 0 个槽位的起点，不能任意改为当前时间。
 * - 插入、删除和轮询均以 deadline - _start 计算时间偏移。
 * - 主要操作接口为 add_task、remove_task 和 poll。
 */

class Timer;

/** @brief 运行时定时器。 */
class Timer {
 public:
  /**
   * @brief stop_callback 可从其他线程调用；只记录请求，时间轮本身仍由所属 worker
   * 修改。
   */
  void request_prune() noexcept { _cancel_requests.fetch_add(1, std::memory_order_release); }

  /** @brief 禁止复制构造运行时定时器。 */
  Timer(const Timer&) = delete;

  /** @brief 禁止复制赋值。 */
  Timer& operator=(const Timer&) = delete;

  /** @brief 禁止移动构造运行时定时器。 */
  Timer(Timer&&) = delete;

  /** @brief 禁止移动赋值。 */
  Timer& operator=(Timer&&) = delete;

  /** @brief 构造运行时定时器。 */
  explicit Timer(bool bind_thread = true) {
    if (bind_thread)
      ::faio::detail::current_thread_context().io.timer = this;
    faio::log::logger()->debug("Timer: initialized at thread");
  }

  /** @brief 销毁运行时定时器。 */
  ~Timer() {
    if (auto* context = ::faio::detail::try_current_thread_context();
        context && context->io.timer == this)
      context->io.timer = nullptr;
    faio::log::logger()->debug("Timer: destroyed, entries remaining={}", _num_entries);
  }

 public:
  /**
   * @brief 添加定时器任务
   * @param deadline 任务的绝对到期时间
   * @param handle 任务到期时要恢复的协程句柄
   * @return 任务的裸指针（用于后续移除操作），调用者不拥有所有权
   */
  auto add_task(std::chrono::steady_clock::time_point deadline,
                std::coroutine_handle<> handle,
                std::shared_ptr<std::atomic<unsigned char>> claim = {}) -> TimerTask* {
    auto task = std::make_unique<TimerTask>(deadline, handle, std::move(claim));
    auto* raw = task.get();
    add_task_impl(std::move(task));
    return raw;
  }

  /** @brief sleep 捕获的恢复目标；到期与取消使用同一份调度约束。 */
  template <class S>
  auto add_task(std::chrono::steady_clock::time_point deadline,
                std::coroutine_handle<> handle,
                std::shared_ptr<std::atomic<unsigned char>> claim,
                ::faio::detail::basic_resume_target<S> target) -> TimerTask* {
    auto task = std::make_unique<TimerTask>(deadline, handle, std::move(claim), target);
    auto* raw = task.get();
    add_task_impl(std::move(task));
    return raw;
  }

  /**
   * @brief 移除定时器任务
   * @param task 要移除的任务裸指针
   */
  void remove_task(TimerTask* task) {
    if (task == nullptr) {
      return;
    }

    auto now = std::chrono::steady_clock::now();
    if (task->_deadline <= now) {
      return;
    }

    auto interval_ms = to_ms(task->_deadline - _start);

    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              wheel_ptr->remove_task(task, interval_ms);
              if (_num_entries > 0) {
                --_num_entries;
              }
              try_level_down();
            }
          }
        },
        _root_wheel);
  }

  /**
   * @brief 轮询处理到期任务
   * @param sink 所属线程的就绪接收端
   * @return 本次处理的到期任务数量
   */
  template <ready_sink sink_type>
  auto poll(sink_type& sink) -> std::size_t {
    if (_cancel_requests.load(std::memory_order_acquire) != 0
        && _cancel_requests.exchange(0, std::memory_order_acq_rel) != 0) {
      std::size_t removed = 0;
      std::visit(
          [&](auto& wheel_ptr) {
            using T = std::decay_t<decltype(wheel_ptr)>;
            if constexpr (!std::is_same_v<T, std::monostate>)
              if (wheel_ptr)
                removed = wheel_ptr->prune_cancelled();
          },
          _root_wheel);
      _num_entries -= std::min(_num_entries, removed);
      if (removed)
        try_level_down();
    }
    if (_num_entries == 0) {
      return 0;
    }

    auto elapsed = elapsed_ms();
    if (elapsed == 0) {
      return 0;
    }

    std::size_t count = 0;
    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              wheel_ptr->handle_expired_tasks(sink, count, elapsed);
            }
          }
        },
        _root_wheel);

    advance_start(elapsed);

    if (count > 0) {
      _num_entries -= std::min(_num_entries, count);
      try_level_down();

      faio::log::logger()->trace(
          "Timer::poll: processed {} tasks, {} remaining", count, _num_entries);
    }

    return count;
  }

  /**
   * @brief 取得下一个任务距 now 的到期时间，单位为毫秒。
   * @return 取得下一个到期任务距离现在的毫秒数；无任务时返回 max。
   */
  [[nodiscard]]
  auto next_deadline_ms() const noexcept -> std::size_t {
    if (_num_entries == 0) {
      return std::numeric_limits<std::size_t>::max();
    }

    std::size_t expected = std::numeric_limits<std::size_t>::max();
    std::visit(
        [&](const auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              auto deadline_from_start = wheel_ptr->next_deadline_time();
              auto elapsed = elapsed_ms();
              if (deadline_from_start > elapsed) {
                expected = deadline_from_start - elapsed;
              } else {
                expected = 0;
              }
            }
          }
        },
        _root_wheel);

    return expected;
  }

  /** @brief 取得尚未完成的定时器任务数。 */
  [[nodiscard]]
  auto num_entries() const noexcept -> std::size_t {
    return _num_entries;
  }

  /** @brief 判断定时器是否没有待处理任务。 */
  [[nodiscard]]
  auto empty() const noexcept -> bool {
    return _num_entries == 0;
  }

 private:
  /** @brief 内部添加任务的统一实现 */
  void add_task_impl(std::unique_ptr<TimerTask>&& task) {
    if (_num_entries == 0) {
      _start = std::chrono::steady_clock::now();
      _root_wheel = std::monostate{};
    }

    auto interval_ms = to_ms(task->_deadline - _start);

    ensure_capacity(interval_ms);

    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              wheel_ptr->add_task(std::move(task), interval_ms);
            }
          }
        },
        _root_wheel);

    ++_num_entries;
  }

  /**
   * @brief 确保根时间轮的容量足够容纳 interval_ms
   * 如果当前层级不够，自动升级（level_up）
   */
  void ensure_capacity(std::size_t interval_ms) {
    if (std::holds_alternative<std::monostate>(_root_wheel)) {
      if (interval_ms < TimerWheel<0uz>::SPAN_MS) {
        _root_wheel = std::make_unique<TimerWheel<0uz>>();
        return;
      }
      _root_wheel = std::make_unique<TimerWheel<0uz>>();
    }

    ensure_capacity_visit(interval_ms);
  }

  /** @brief 利用 variant visit 实现递归升级 */
  void ensure_capacity_visit(std::size_t interval_ms) {
    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              using WheelType = std::remove_reference_t<decltype(*wheel_ptr)>;
              if (interval_ms >= WheelType::SPAN_MS) {
                level_up_variant(interval_ms);
              }
            }
          }
        },
        _root_wheel);
  }

  /** @brief 将根时间轮升级一级 */
  void level_up_variant(std::size_t interval_ms) {
    auto idx = _root_wheel.index();

    level_up_dispatch(interval_ms, idx);
  }

  /** @brief 编译期分发升级逻辑的辅助函数 */
  void level_up_dispatch(std::size_t interval_ms, std::size_t current_idx) {
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (
          [&] {
            constexpr std::size_t LEVEL = Is;
            constexpr std::size_t VARIANT_IDX = LEVEL + 1;
            if (current_idx == VARIANT_IDX) {
              if constexpr (LEVEL < MAX_LEVEL) {
                auto& current_wheel = std::get<VARIANT_IDX>(_root_wheel);
                auto new_wheel = current_wheel->level_up(std::move(current_wheel));
                _root_wheel = std::move(new_wheel);

                ensure_capacity_visit(interval_ms);
              } else {
                faio::log::logger()->error("Timer: cannot level_up beyond MAX_LEVEL={}", MAX_LEVEL);
              }
            }
          }(),
          ...);
    }(std::make_index_sequence<MAX_LEVEL + 1>{});
  }

  /**
   * @brief 尝试降级根时间轮
   * 当只有第 0 个槽位有子轮时，可以降级以节省内存
   * TimerWheel<0> 是最底层，不可再降级
   */
  void try_level_down() {
    auto idx = _root_wheel.index();
    if (idx <= 1) {
      if (idx == 1) {
        auto& wheel_ptr = std::get<1>(_root_wheel);
        if (wheel_ptr && wheel_ptr->empty()) {
          wheel_ptr.reset();
          _root_wheel = std::monostate{};
        }
      }
      return;
    }

    level_down_dispatch();
  }

  /** @brief 编译期分发降级逻辑 */
  void level_down_dispatch() {
    auto idx = _root_wheel.index();
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (
          [&] {
            constexpr std::size_t LEVEL = Is;
            constexpr std::size_t VARIANT_IDX = LEVEL + 1;
            if constexpr (LEVEL >= 1 && LEVEL <= MAX_LEVEL) {
              if (idx == VARIANT_IDX) {
                auto& wheel_ptr = std::get<VARIANT_IDX>(_root_wheel);
                if (!wheel_ptr) {
                  return;
                }
                if (wheel_ptr->empty()) {
                  wheel_ptr.reset();
                  _root_wheel = std::monostate{};
                } else if (wheel_ptr->can_level_down()) {
                  auto child = wheel_ptr->level_down();
                  if (child) {
                    _root_wheel = std::move(child);
                    try_level_down();
                  }
                }
              }
            }
          }(),
          ...);
    }(std::make_index_sequence<MAX_LEVEL + 1>{});
  }

  /**
   * @brief 推进时间基准
   * @param ms 从当前根轮起点起已经经过的毫秒数。
   * @details 高层时间轮只能按完整子轮的跨度推进。部分扫描的子轮中，已扫描的前缀保持为空，
   * 剩余任务的索引仍以该子轮原来的起点为基准，不能减去已扫描的部分跨度。
   */
  void advance_start(std::size_t ms) {
    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              using WheelType = std::remove_reference_t<decltype(*wheel_ptr)>;
              if constexpr (std::is_same_v<WheelType, TimerWheel<0uz>>) {
                wheel_ptr->rotate(ms);
                _start += std::chrono::milliseconds(ms);
              } else {
                auto slots_to_rotate = ms >> WheelType::CHILD_SHIFT;
                if (slots_to_rotate > 0) {
                  wheel_ptr->rotate(slots_to_rotate);
                  _start += std::chrono::milliseconds(slots_to_rotate << WheelType::CHILD_SHIFT);
                }
              }
            }
          }
        },
        _root_wheel);
  }

  /** @brief 计算自启动以来经过的毫秒数 */
  [[nodiscard]]
  auto elapsed_ms() const noexcept -> std::size_t {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - _start);
    return static_cast<std::size_t>(duration.count());
  }

  /** @brief 将 duration 转换为毫秒数 */
  static auto to_ms(std::chrono::steady_clock::duration dur) noexcept -> std::size_t {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur);
    return static_cast<std::size_t>(std::max(ms.count(), decltype(ms)::rep{0}));
  }

 private:
  std::chrono::steady_clock::time_point _start{
      std::chrono::steady_clock::
          now()};              /**< 当前根轮第 0 槽的基准时间，始终与该层级的旋转跨度一致。 */
  std::size_t _num_entries{0}; /**< 当前活跃的定时器任务数 */
  std::atomic<std::size_t> _cancel_requests{
      0}; /**< 跨线程取消请求计数，所属线程据此按需清理时间轮。 */
  VariantWheelBuilder<MAX_LEVEL + 1uz>::Type
      _root_wheel{}; /**< 根时间轮（variant 存储，支持不同层级） */
};
}  // namespace faio::runtime::detail::timer
#endif
