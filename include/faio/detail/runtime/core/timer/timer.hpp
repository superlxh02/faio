#ifndef FAIO_DETAIL_RUNTIME_CORE_TIMER_TIMER_HPP
#define FAIO_DETAIL_RUNTIME_CORE_TIMER_TIMER_HPP

#include "faio/detail/runtime/core/config.hpp"
#include "faio/detail/runtime/core/timer/task.hpp"
#include "faio/detail/runtime/core/timer/wheel.hpp"
#include "fastlog/fastlog.hpp"
#include <chrono>
#include <variant>

namespace faio::runtime::detail::timer {

// =========================================================================
// VariantWheelBuilder：利用 std::variant 构造能存放任意层级时间轮的类型
// 生成 std::variant<std::monostate, unique_ptr<TimerWheel<0>>,
//                    unique_ptr<TimerWheel<1>>, ...,
//                    unique_ptr<TimerWheel<N-1>>>
// =========================================================================
template <std::size_t... N>
static inline constexpr auto variant_wheel_impl(std::index_sequence<N...>) {
  return std::variant<std::monostate, std::unique_ptr<TimerWheel<N>>...>{};
}

template <std::size_t N> struct VariantWheelBuilder {
  using Type = decltype(variant_wheel_impl(std::make_index_sequence<N>{}));
};

// =========================================================================
// Timer 类 —— 对外暴露的定时器管理器
//
// 核心设计：
//   - 每个 worker 线程拥有一个独立的 Timer 实例（通过 thread_local 指针）
//   - 内部用 variant 存放不同层级的根时间轮，实现动态升降级
//   - _start 记录定时器创建时的基准时间点
//   - 通过 elapsed_ms() 计算自启动以来经过的毫秒数
//   - add_task / remove_task / poll 为主要操作接口
// =========================================================================
class Timer;
inline thread_local Timer *current_timer;

class Timer {
public:
  Timer(const Timer &) = delete;
  Timer &operator=(const Timer &) = delete;
  Timer(Timer &&) = delete;
  Timer &operator=(Timer &&) = delete;

  Timer() {
    current_timer = this;
    fastlog::console.debug("Timer: initialized at thread");
  }

  ~Timer() {
    current_timer = nullptr;
    fastlog::console.debug("Timer: destroyed, entries remaining={}",
                           _num_entries);
  }

public:
  /// 添加定时器任务
  /// @param deadline 任务的绝对到期时间
  /// @param handle 任务到期时要恢复的协程句柄
  /// @return 任务的裸指针（用于后续移除操作），调用者不拥有所有权
  auto add_task(std::chrono::steady_clock::time_point deadline,
                std::coroutine_handle<> handle) -> TimerTask * {
    auto task = std::make_unique<TimerTask>(deadline, handle);
    auto *raw = task.get();
    add_task_impl(std::move(task));
    return raw;
  }

  /// 添加定时器任务（io_user_data 版本，用于 io_uring 超时取消）
  /// @param deadline 任务的绝对到期时间
  /// @param user_data io_uring 用户数据指针
  /// @return 任务的裸指针
  auto add_task(std::chrono::steady_clock::time_point deadline,
                io::detail::io_user_data_t *user_data) -> TimerTask * {
    auto task = std::make_unique<TimerTask>(deadline, user_data);
    auto *raw = task.get();
    add_task_impl(std::move(task));
    return raw;
  }

  /// 移除定时器任务
  /// @param task 要移除的任务裸指针
  void remove_task(TimerTask *task) {
    if (task == nullptr) {
      return;
    }

    auto now = std::chrono::steady_clock::now();
    // 如果任务已过期，无需移除
    if (task->_deadline <= now) {
      return;
    }

    auto interval_ms = to_ms(task->_deadline - _start);
    auto elapsed = elapsed_ms();
    if (interval_ms <= elapsed) {
      return;
    }
    auto relative_interval = interval_ms - elapsed;

    // 在根时间轮中递归移除
    std::visit(
        [&](auto &wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              wheel_ptr->remove_task(task, relative_interval);
              if (_num_entries > 0) {
                --_num_entries;
              }
              try_level_down();
            }
          }
        },
        _root_wheel);
  }

  /// 轮询处理到期任务
  /// @param local_queue 本地任务队列
  /// @param global_queue 全局任务队列
  /// @return 本次处理的到期任务数量
  template <typename LocalQueue, typename GlobalQueue>
  auto poll(LocalQueue &local_queue, GlobalQueue &global_queue) -> std::size_t {
    if (_num_entries == 0) {
      return 0;
    }

    auto elapsed = elapsed_ms();
    if (elapsed == 0) {
      return 0;
    }

    std::size_t count = 0;
    std::visit(
        [&](auto &wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              wheel_ptr->handle_expired_tasks(local_queue, global_queue, count,
                                              elapsed);
            }
          }
        },
        _root_wheel);

    // 无论是否有任务被处理，都推进时间基准以保持时钟一致性
    advance_start(elapsed);

    if (count > 0) {
      _num_entries -= std::min(_num_entries, count);
      // 尝试降级
      try_level_down();

      fastlog::console.trace("Timer::poll: processed {} tasks, {} remaining",
                             count, _num_entries);
    }

    return count;
  }

  /// 获取下一个到期任务的时间（距 now 的毫秒数）
  /// @return 下一个到期任务距离现在的毫秒数；若无任务返回 max
  [[nodiscard]]
  auto next_deadline_ms() const noexcept -> std::size_t {
    if (_num_entries == 0) {
      return std::numeric_limits<std::size_t>::max();
    }

    std::size_t expected = std::numeric_limits<std::size_t>::max();
    std::visit(
        [&](const auto &wheel_ptr) {
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

  /// 获取当前剩余定时器任务数
  [[nodiscard]]
  auto num_entries() const noexcept -> std::size_t {
    return _num_entries;
  }

  /// 判断定时器是否为空（没有待处理任务）
  [[nodiscard]]
  auto empty() const noexcept -> bool {
    return _num_entries == 0;
  }

private:
  /// 内部添加任务的统一实现
  void add_task_impl(std::unique_ptr<TimerTask> &&task) {
    // 计算任务距离启动时间的间隔
    auto interval_ms = to_ms(task->_deadline - _start);
    auto elapsed = elapsed_ms();

    // 如果任务已经过期或即将过期，调整到最小间隔
    std::size_t relative_interval = 0;
    if (interval_ms > elapsed) {
      relative_interval = interval_ms - elapsed;
    }

    // 确保根时间轮已初始化，且层级足够容纳此任务
    ensure_capacity(relative_interval);

    // 将任务添加到根时间轮
    std::visit(
        [&](auto &wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              wheel_ptr->add_task(std::move(task), relative_interval);
            }
          }
        },
        _root_wheel);

    ++_num_entries;
  }

  /// 确保根时间轮的容量足够容纳 interval_ms
  /// 如果当前层级不够，自动升级（level_up）
  void ensure_capacity(std::size_t interval_ms) {
    // 如果根时间轮还没初始化，创建 level-0 时间轮
    if (std::holds_alternative<std::monostate>(_root_wheel)) {
      if (interval_ms < TimerWheel<0uz>::SPAN_MS) {
        _root_wheel = std::make_unique<TimerWheel<0uz>>();
        return;
      }
      // 需要更高级别的时间轮
      _root_wheel = std::make_unique<TimerWheel<0uz>>();
    }

    // 递归升级直到容量足够
    ensure_capacity_visit(interval_ms);
  }

  /// 利用 variant visit 实现递归升级
  void ensure_capacity_visit(std::size_t interval_ms) {
    std::visit(
        [&](auto &wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              using WheelType = std::remove_reference_t<decltype(*wheel_ptr)>;
              // 如果当前轮的跨度不足以容纳 interval_ms，需要升级
              if (interval_ms >= WheelType::SPAN_MS) {
                level_up_variant(interval_ms);
              }
            }
          }
        },
        _root_wheel);
  }

  /// 将根时间轮升级一级
  void level_up_variant(std::size_t interval_ms) {
    // 使用 index 实现编译期分发升级逻辑
    // variant 的 index 0 是 monostate，1 对应 TimerWheel<0>,
    // 2 对应 TimerWheel<1>, ...
    auto idx = _root_wheel.index();

    // 利用编译期索引序列实现升级分发
    level_up_dispatch(interval_ms, idx);
  }

  /// 编译期分发升级逻辑的辅助函数
  void level_up_dispatch(std::size_t interval_ms, std::size_t current_idx) {
    // 逐级升级，MAX_LEVEL 为上限
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (
          [&] {
            constexpr std::size_t LEVEL = Is;
            constexpr std::size_t VARIANT_IDX = LEVEL + 1; // +1 因为 monostate
            if (current_idx == VARIANT_IDX) {
              if constexpr (LEVEL < MAX_LEVEL) {
                auto &current_wheel = std::get<VARIANT_IDX>(_root_wheel);
                auto new_wheel =
                    current_wheel->level_up(std::move(current_wheel));
                _root_wheel = std::move(new_wheel);

                // 继续检查是否需要再升级
                ensure_capacity_visit(interval_ms);
              } else {
                fastlog::console.error(
                    "Timer: cannot level_up beyond MAX_LEVEL={}", MAX_LEVEL);
              }
            }
          }(),
          ...);
    }(std::make_index_sequence<MAX_LEVEL + 1>{});
  }

  /// 尝试降级根时间轮
  /// 当只有第 0 个槽位有子轮时，可以降级以节省内存
  /// TimerWheel<0> 是最底层，不可再降级
  void try_level_down() {
    auto idx = _root_wheel.index();
    // monostate (idx==0) 或 TimerWheel<0> (idx==1) 不需要降级
    if (idx <= 1) {
      // 对 TimerWheel<0>，仅检查是否为空可以释放
      if (idx == 1) {
        auto &wheel_ptr = std::get<1>(_root_wheel);
        if (wheel_ptr && wheel_ptr->empty()) {
          wheel_ptr.reset();
          _root_wheel = std::monostate{};
        }
      }
      return;
    }

    // 对 LEVEL >= 1 的时间轮，尝试降级
    level_down_dispatch();
  }

  /// 编译期分发降级逻辑
  void level_down_dispatch() {
    auto idx = _root_wheel.index();
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (
          [&] {
            constexpr std::size_t LEVEL = Is;
            constexpr std::size_t VARIANT_IDX = LEVEL + 1;
            if constexpr (LEVEL >= 1 && LEVEL <= MAX_LEVEL) {
              if (idx == VARIANT_IDX) {
                auto &wheel_ptr = std::get<VARIANT_IDX>(_root_wheel);
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
                    // 递归检查是否可以继续降级
                    try_level_down();
                  }
                }
              }
            }
          }(),
          ...);
    }(std::make_index_sequence<MAX_LEVEL + 1>{});
  }

  /// 推进时间基准
  /// 旋转根时间轮以匹配新的起始时间
  void advance_start(std::size_t ms) {
    _start += std::chrono::milliseconds(ms);

    // 旋转根时间轮
    std::visit(
        [&](auto &wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              using WheelType = std::remove_reference_t<decltype(*wheel_ptr)>;
              // 对于 level-0 轮直接旋转
              if constexpr (std::is_same_v<WheelType, TimerWheel<0uz>>) {
                wheel_ptr->rotate(ms);
              } else {
                // 对于高层级轮，计算需要旋转的槽位数（移位代替除法）
                auto slots_to_rotate = ms >> WheelType::CHILD_SHIFT;
                if (slots_to_rotate > 0) {
                  wheel_ptr->rotate(slots_to_rotate);
                }
              }
            }
          }
        },
        _root_wheel);
  }

  /// 计算自启动以来经过的毫秒数
  [[nodiscard]]
  auto elapsed_ms() const noexcept -> std::size_t {
    auto now = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - _start);
    return static_cast<std::size_t>(duration.count());
  }

  /// 将 duration 转换为毫秒数
  static auto to_ms(std::chrono::steady_clock::duration dur) noexcept
      -> std::size_t {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur);
    return static_cast<std::size_t>(std::max(ms.count(), decltype(ms)::rep{0}));
  }

private:
  /// 定时器启动的基准时间
  std::chrono::steady_clock::time_point _start{
      std::chrono::steady_clock::now()};
  /// 当前活跃的定时器任务数
  std::size_t _num_entries{0};
  /// 根时间轮（variant 存储，支持不同层级）
  VariantWheelBuilder<MAX_LEVEL + 1uz>::Type _root_wheel{};
};

} // namespace faio::runtime::detail::timer
#endif // FAIO_DETAIL_RUNTIME_CORE_TIMER_TIMER_HPP
