#ifndef FAIO_DETAIL_RUNTIME_TIMER_TIMER_HPP
#define FAIO_DETAIL_RUNTIME_TIMER_TIMER_HPP

#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/timer/task.hpp"
#include "faio/detail/runtime/timer/wheel.hpp"
#include "faio/log.hpp"
#include <atomic>
#include <chrono>
#include <variant>

namespace faio::runtime::detail::timer {
// =========================================================================
// VariantWheelBuilder：利用 std::variant 构造能存放任意层级时间轮的类型
// 生成 std::variant<std::monostate, unique_ptr<TimerWheel<0>>,
// 后续类型为 unique_ptr<TimerWheel<1>>, ...,
// 最后一个类型为 unique_ptr<TimerWheel<N-1>>>
// =========================================================================
template <std::size_t... N>
static inline constexpr auto variant_wheel_impl(std::index_sequence<N...>) {
  return std::variant<std::monostate, std::unique_ptr<TimerWheel<N>>...>{};
}

template <std::size_t N>
struct VariantWheelBuilder {
  using Type = decltype(variant_wheel_impl(std::make_index_sequence<N>{}));
};

// =========================================================================
// Timer 类 —— 对外暴露的定时器管理器
//
// 核心设计：
//   - 每个 worker 线程拥有一个独立的 Timer 实例（通过 thread_local 指针）
//   - 内部用 variant 存放不同层级的根时间轮，实现动态升降级
//   - _start 始终对应根轮第 0 个槽位的起点，不能任意改为当前时间
//   - 插入、删除、轮询都使用 deadline - _start 这一坐标系
//   - add_task / remove_task / poll 为主要操作接口
// =========================================================================
class Timer;
inline thread_local Timer* current_timer;

class Timer {
 public:
  // stop_callback 可从其他线程调用；只记录请求，时间轮本身仍由所属 worker
  // 修改。
  void request_prune() noexcept { _cancel_requests.fetch_add(1, std::memory_order_release); }

  Timer(const Timer&) = delete;

  Timer& operator=(const Timer&) = delete;

  Timer(Timer&&) = delete;

  Timer& operator=(Timer&&) = delete;

  explicit Timer(bool bind_thread = true) {
    if (bind_thread)
      current_timer = this;
    faio::log::logger()->debug("Timer: initialized at thread");
  }

  ~Timer() {
    if (current_timer == this)
      current_timer = nullptr;
    faio::log::logger()->debug("Timer: destroyed, entries remaining={}", _num_entries);
  }

 public:
  /// 添加定时器任务
  /// @param deadline 任务的绝对到期时间
  /// @param handle 任务到期时要恢复的协程句柄
  /// @return 任务的裸指针（用于后续移除操作），调用者不拥有所有权
  auto add_task(std::chrono::steady_clock::time_point deadline,
                std::coroutine_handle<> handle,
                std::shared_ptr<std::atomic<unsigned char>> claim = {}) -> TimerTask* {
    auto task = std::make_unique<TimerTask>(deadline, handle, std::move(claim));
    auto* raw = task.get();
    add_task_impl(std::move(task));
    return raw;
  }

  /// 移除定时器任务
  /// @param task 要移除的任务裸指针
  void remove_task(TimerTask* task) {
    if (task == nullptr) {
      return;
    }

    auto now = std::chrono::steady_clock::now();
    // 如果任务已过期，无需移除
    if (task->_deadline <= now) {
      return;
    }

    // 所有索引均相对根轮起点；减去 elapsed 会把任务定位到其他槽位。
    auto interval_ms = to_ms(task->_deadline - _start);

    // 在根时间轮中递归移除
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

  /// 轮询处理到期任务
  /// @param sink 所属线程的就绪接收端
  /// @return 本次处理的到期任务数量
  template <ready_sink sink_type>
  auto poll(sink_type& sink) -> std::size_t {
    // 没有取消请求时只读取；避免每次空 IO 轮询都对取消计数执行原子 RMW。
    // load 后新到的请求与原 exchange 后新到的请求一样，由后续 poll 领取。
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

    // 只能推进已完整扫描的根槽位；高层轮的部分子轮仍沿用原来的起点。
    advance_start(elapsed);

    if (count > 0) {
      _num_entries -= std::min(_num_entries, count);
      // 尝试降级
      try_level_down();

      faio::log::logger()->trace(
          "Timer::poll: processed {} tasks, {} remaining", count, _num_entries);
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
  void add_task_impl(std::unique_ptr<TimerTask>&& task) {
    // 空轮没有旧任务约束，可以重新建立基准，避免长时间空闲扩大层级。
    if (_num_entries == 0) {
      _start = std::chrono::steady_clock::now();
      _root_wheel = std::monostate{};
    }

    // 活跃轮中即使 worker 很久没有 poll，也必须按旧根起点计算绝对槽位。
    // 截止时间落在当前时刻之前时使用 0；下一次 poll 会处理该已过期任务。
    auto interval_ms = to_ms(task->_deadline - _start);

    // 确保根时间轮已初始化，且层级足够容纳此任务
    ensure_capacity(interval_ms);

    // 将任务添加到根时间轮
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
        [&](auto& wheel_ptr) {
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
            constexpr std::size_t VARIANT_IDX = LEVEL + 1;  // +1 因为 monostate
            if (current_idx == VARIANT_IDX) {
              if constexpr (LEVEL < MAX_LEVEL) {
                auto& current_wheel = std::get<VARIANT_IDX>(_root_wheel);
                auto new_wheel = current_wheel->level_up(std::move(current_wheel));
                _root_wheel = std::move(new_wheel);

                // 继续检查是否需要再升级
                ensure_capacity_visit(interval_ms);
              } else {
                faio::log::logger()->error("Timer: cannot level_up beyond MAX_LEVEL={}", MAX_LEVEL);
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
        auto& wheel_ptr = std::get<1>(_root_wheel);
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
  /// @param ms 从当前根轮起点起已经经过的毫秒数。
  /// @details 高层轮只能移动完整子轮的跨度。部分子轮已经扫描的前缀保持为空，
  ///          但它内部剩余任务的索引仍以原子轮起点为基准，不能减掉部分跨度。
  void advance_start(std::size_t ms) {
    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          if constexpr (!std::is_same_v<T, std::monostate>) {
            if (wheel_ptr) {
              using WheelType = std::remove_reference_t<decltype(*wheel_ptr)>;
              // 叶子层每槽 1ms，所有已扫描毫秒都可以从根坐标中移除。
              if constexpr (std::is_same_v<WheelType, TimerWheel<0uz>>) {
                wheel_ptr->rotate(ms);
                _start += std::chrono::milliseconds(ms);
              } else {
                // 向下取整到完整子轮，保留 partial child 的内部坐标。
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

  /// 计算自启动以来经过的毫秒数
  [[nodiscard]]
  auto elapsed_ms() const noexcept -> std::size_t {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - _start);
    return static_cast<std::size_t>(duration.count());
  }

  /// 将 duration 转换为毫秒数
  static auto to_ms(std::chrono::steady_clock::duration dur) noexcept -> std::size_t {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur);
    return static_cast<std::size_t>(std::max(ms.count(), decltype(ms)::rep{0}));
  }

 private:
  /// 当前根轮第 0 槽的基准时间，始终与该层级的旋转跨度一致。
  std::chrono::steady_clock::time_point _start{std::chrono::steady_clock::now()};
  /// 当前活跃的定时器任务数
  std::size_t _num_entries{0};
  std::atomic<std::size_t> _cancel_requests{0};
  /// 根时间轮（variant 存储，支持不同层级）
  VariantWheelBuilder<MAX_LEVEL + 1uz>::Type _root_wheel{};
};
}  // namespace faio::runtime::detail::timer
#endif
