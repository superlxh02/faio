#ifndef FAIO_DETAIL_RUNTIME_CORE_TIMER_TIMERWHEEL_HPP
#define FAIO_DETAIL_RUNTIME_CORE_TIMER_TIMERWHEEL_HPP

#include "faio/detail/common/static_math.hpp"
#include "faio/detail/runtime/core/config.hpp"
#include "faio/detail/runtime/core/timer/task.hpp"
#include "fastlog/fastlog.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace faio::runtime::detail::timer {
// =========================================================================
// TimerWheel<LEVEL> 通用模板 —— 多级时间轮（LEVEL >= 1）
//
// 层级结构：
//   LEVEL 0: 底层，每槽 1ms，范围 64ms
//   LEVEL 1: 每槽管辖 64ms（一个 LEVEL-0 轮），范围 64*64 = 4096ms ≈ 4s
//   LEVEL 2: 每槽管辖 4096ms，范围 64^3 = 262144ms ≈ 4.4min
//   LEVEL N: 每槽管辖 64^N ms，范围 64^(N+1) ms
//
// 每个非底层时间轮的槽位存储一个子时间轮指针。
// 使用 bitmap (_slot_map) 加速非空槽位查找。
// =========================================================================
template <std::size_t LEVEL> class TimerWheel {
  static_assert(
      LEVEL >= 1,
      "LEVEL must be >= 1, use TimerWheel<0> specialization for level 0");

public:
  using child_wheel = TimerWheel<LEVEL - 1>;            // 子时间轮类型
  using child_wheel_ptr = std::unique_ptr<child_wheel>; // 子时间轮指针

  // 子层级一个完整轮的时间跨度（ms），等于 64^LEVEL
  static constexpr std::size_t CHILD_SPAN_MS =
      util::static_pow(SLOT_SIZE, LEVEL);
  // 当前层级整个时间轮的总跨度（ms），等于 64^(LEVEL+1)
  static constexpr std::size_t SPAN_MS =
      util::static_pow(SLOT_SIZE, LEVEL + 1uz);
  // CHILD_SPAN_MS 的移位量：log2(CHILD_SPAN_MS) = SLOT_SHIFT * LEVEL
  static constexpr std::size_t CHILD_SHIFT = SLOT_SHIFT * LEVEL;
  // CHILD_SPAN_MS 的掩码：CHILD_SPAN_MS - 1
  static constexpr std::size_t CHILD_MASK = CHILD_SPAN_MS - 1uz;

public:
  TimerWheel() = default;

  /// 由一个已有的子时间轮升级构造
  /// @param child 要放入第0号槽位的子时间轮
  explicit TimerWheel(child_wheel_ptr &&child) : _slot_map(1ull) {
    _wheels_slots[0] = std::move(child);
  }

  ~TimerWheel() = default;

  TimerWheel(const TimerWheel &) = delete;
  TimerWheel &operator=(const TimerWheel &) = delete;
  TimerWheel(TimerWheel &&) = default;
  TimerWheel &operator=(TimerWheel &&) = default;

public:
  /// 添加定时器任务到合适的子时间轮槽位
  /// @param task 待添加的定时器任务
  /// @param interval 距当前位置的时间间隔（ms）
  void add_task(std::unique_ptr<TimerTask> &&task, std::size_t interval) {
    // 计算任务应该落在哪个槽位（右移代替除法）
    auto slot_idx = interval >> CHILD_SHIFT;
    if (slot_idx >= SLOT_SIZE) {
      fastlog::console.error(
          "TimerWheel<{}>::add_task: interval {} exceeds wheel span, "
          "slot_idx={}, clamping to last slot",
          LEVEL, interval, slot_idx);
      slot_idx = SLOT_SIZE - 1;
    }

    // 如果该槽位的子时间轮尚未创建，则懒创建
    if (_wheels_slots[slot_idx] == nullptr) {
      _wheels_slots[slot_idx] = std::make_unique<child_wheel>();
      _slot_map |= (1ull << slot_idx);
    }

    // 计算在子时间轮内的偏移量（掩码代替取余）
    auto child_interval = interval & CHILD_MASK;
    _wheels_slots[slot_idx]->add_task(std::move(task), child_interval);
  }

  /// 从子时间轮中移除定时器任务
  /// @param task 待移除的任务指针
  /// @param interval 任务所在的时间间隔
  void remove_task(TimerTask *task, std::size_t interval) {
    auto slot_idx = interval >> CHILD_SHIFT;
    if (slot_idx >= SLOT_SIZE || _wheels_slots[slot_idx] == nullptr) {
      fastlog::console.warn(
          "TimerWheel<{}>::remove_task: invalid slot {} or empty child wheel",
          LEVEL, slot_idx);
      return;
    }

    auto child_interval = interval & CHILD_MASK;
    _wheels_slots[slot_idx]->remove_task(task, child_interval);

    // 如果子时间轮变空了，释放并清除 bitmap 标记
    if (_wheels_slots[slot_idx]->empty()) {
      _wheels_slots[slot_idx].reset();
      _slot_map &= ~(1ull << slot_idx);
    }
  }

  /// 处理到期任务：递归处理子时间轮中的到期任务
  /// @param local_queue 本地任务队列
  /// @param global_queue 全局任务队列
  /// @param count 已处理任务计数（累加）
  /// @param remaining_ms 剩余需要处理的毫秒数
  template <typename LocalQueue, typename GlobalQueue>
  void handle_expired_tasks(LocalQueue &local_queue, GlobalQueue &global_queue,
                            std::size_t &count, std::size_t remaining_ms) {
    if (_slot_map == 0 || remaining_ms == 0) {
      return;
    }

    // 计算需要完整扫描的槽位数（移位代替除法，掩码代替取余）
    auto full_slots = remaining_ms >> CHILD_SHIFT;
    auto partial_remaining = remaining_ms & CHILD_MASK;

    // 处理需要完整清空的槽位
    for (std::size_t i = 0; i < full_slots && i < SLOT_SIZE; ++i) {
      if ((_slot_map & (1ull << i)) == 0) {
        continue;
      }
      // 完整处理整个子时间轮
      _wheels_slots[i]->handle_expired_tasks(local_queue, global_queue, count,
                                             child_wheel::SPAN_MS);
      _wheels_slots[i].reset();
      _slot_map &= ~(1ull << i);
    }

    // 处理部分到期的槽位（第 full_slots 个槽位可能只有部分到期）
    if (partial_remaining > 0 && full_slots < SLOT_SIZE) {
      if ((_slot_map & (1ull << full_slots)) != 0) {
        _wheels_slots[full_slots]->handle_expired_tasks(
            local_queue, global_queue, count, partial_remaining);

        // 如果子时间轮处理后变空，清理
        if (_wheels_slots[full_slots]->empty()) {
          _wheels_slots[full_slots].reset();
          _slot_map &= ~(1ull << full_slots);
        }
      }
    }
  }

  /// 获取最近到期任务的时间偏移（ms）
  /// @return 距当前位置最近的任务的时间偏移
  [[nodiscard]]
  auto next_deadline_time() const noexcept -> std::size_t {
    if (_slot_map == 0) {
      return SPAN_MS;
    }
    // 找到最低非空槽位
    auto first_slot = static_cast<std::size_t>(std::countr_zero(_slot_map));

    // 递归查询子时间轮中最近到期的时间
    auto child_deadline = _wheels_slots[first_slot]->next_deadline_time();
    return (first_slot << CHILD_SHIFT) + child_deadline;
  }

  /// 旋转时间轮：推进 start 个子时间轮的距离
  /// 前 start 个槽位被丢弃，后面的前移
  /// @param start 要推进的槽位数
  void rotate(std::size_t start) {
    if (start == 0 || start >= SLOT_SIZE) {
      return;
    }
    // 位图右移
    _slot_map >>= start;
    // 槽位数组前移
    for (std::size_t i = 0; i < SLOT_SIZE - start; ++i) {
      _wheels_slots[i] = std::move(_wheels_slots[i + start]);
    }
    // 清空尾部
    for (std::size_t i = SLOT_SIZE - start; i < SLOT_SIZE; ++i) {
      _wheels_slots[i].reset();
    }
  }

  /// 升级到更高层级的时间轮
  /// 当前时间轮变为父时间轮第0个槽位的子轮
  /// 仅当 LEVEL < MAX_LEVEL 时可用（最高层无法再升级）
  /// @param me 当前时间轮的 unique_ptr（所有权转移）
  /// @return 新创建的父时间轮
  [[nodiscard]]
  auto level_up(std::unique_ptr<TimerWheel> &&me)
      -> std::unique_ptr<TimerWheel<LEVEL + 1>>
    requires(LEVEL < MAX_LEVEL)
  {
    fastlog::console.debug("TimerWheel<{}>::level_up: upgrading to level {}",
                           LEVEL, LEVEL + 1);
    return std::make_unique<TimerWheel<LEVEL + 1>>(std::move(me));
  }

  /// 降级到子时间轮
  /// 取出第0号槽位的子时间轮并返回
  /// @return 第0号子时间轮的 unique_ptr
  [[nodiscard]]
  auto level_down() -> child_wheel_ptr {
    if (_wheels_slots[0] == nullptr) {
      return nullptr;
    }
    auto child = std::move(_wheels_slots[0]);
    _slot_map &= ~1ull;
    fastlog::console.debug(
        "TimerWheel<{}>::level_down: downgrading to level {}", LEVEL,
        LEVEL - 1);
    return child;
  }

  /// 判断是否可以降级
  /// 条件：只有第0个槽位非空（或整个轮为空）
  [[nodiscard]]
  auto can_level_down() const noexcept -> bool {
    // 只有第0个槽位可能非空，其余槽位必须全空
    return (_slot_map & ~1ull) == 0;
  }

  /// 判断当前时间轮是否为空
  [[nodiscard]]
  auto empty() const noexcept -> bool {
    return _slot_map == 0;
  }

  /// 获取槽位映射 bitmap
  [[nodiscard]]
  auto slot_map() const noexcept -> std::uint64_t {
    return _slot_map;
  }

private:
  /// 子时间轮槽位数组
  std::array<child_wheel_ptr, SLOT_SIZE> _wheels_slots{};
  /// 位图：第 i 位为 1 表示 _wheels_slots[i] 非空
  std::uint64_t _slot_map{0};
};

// =========================================================================
// TimerWheel<0> 特化 —— 最底层时间轮（叶子层）
// 每个槽位存储一个 TimerTask 单链表，直接承载定时器任务。
// SLOT_SIZE = 64 个槽位，每个槽位代表 1ms 的时间粒度。
// =========================================================================
template <> class TimerWheel<0uz> {
public:
  using father_wheel = TimerWheel<1uz>;                   // 父时间轮类型
  using father_wheel_ptr = std::unique_ptr<father_wheel>; // 父时间轮指针

  // 最底层时间轮的时间跨度（ms），每个槽 1ms，共 SLOT_SIZE 个槽
  static constexpr std::size_t SPAN_MS = SLOT_SIZE; // 64ms

public:
  TimerWheel() = default;
  ~TimerWheel() = default;

  TimerWheel(const TimerWheel &) = delete;
  TimerWheel &operator=(const TimerWheel &) = delete;
  TimerWheel(TimerWheel &&) = default;
  TimerWheel &operator=(TimerWheel &&) = default;

public:
  /// 向指定时间间隔的槽位添加定时器任务
  /// @param task 待添加的定时器任务（unique_ptr，所有权转移）
  /// @param interval 从当前位置开始的时间间隔（ms），必须 < SLOT_SIZE
  void add_task(std::unique_ptr<TimerTask> &&task, std::size_t interval) {
    auto slot_idx = interval & SLOT_MASK;
    // 将任务链入该槽位的链表头部
    task->_next = std::move(_task_slots[slot_idx]);
    _task_slots[slot_idx] = std::move(task);
    // 在 bitmap 中标记该槽位非空
    _slot_map |= (1ull << slot_idx);
  }

  /// 从指定槽位中移除定时器任务
  /// @param task 待移除的任务指针（裸指针，用于查找匹配）
  /// @param interval 任务所在的时间间隔
  void remove_task(TimerTask *task, std::size_t interval) {
    auto slot_idx = interval & SLOT_MASK;
    TimerTask *prev = nullptr;
    TimerTask *current = _task_slots[slot_idx].get();

    while (current != nullptr) {
      if (current == task) {
        if (prev == nullptr) {
          // 移除链表头部节点
          _task_slots[slot_idx] = std::move(current->_next);
        } else {
          // 移除中间或尾部节点
          prev->_next = std::move(current->_next);
        }
        // 如果该槽位链表变空，清除 bitmap 标记
        if (_task_slots[slot_idx] == nullptr) {
          _slot_map &= ~(1ull << slot_idx);
        }
        return;
      }
      prev = current;
      current = current->_next.get();
    }
    fastlog::console.warn("TimerWheel<0>::remove_task: task not found in "
                          "slot {}",
                          slot_idx);
  }

  /// 处理到期任务：扫描 [0, remaining_ms) 范围内所有槽位，执行到期任务
  /// @param local_queue 本地任务队列引用（模板参数，适配不同队列类型）
  /// @param global_queue 全局任务队列引用
  /// @param count 已处理任务计数器（输出参数，累加）
  /// @param remaining_ms 剩余需要处理的毫秒数
  template <typename LocalQueue, typename GlobalQueue>
  void handle_expired_tasks(LocalQueue &local_queue, GlobalQueue &global_queue,
                            std::size_t &count, std::size_t remaining_ms) {
    // 计算实际需要扫描的槽位数
    auto slots_to_scan = std::min(remaining_ms, SLOT_SIZE);

    for (std::size_t i = 0; i < slots_to_scan; ++i) {
      // 利用 bitmap 快速跳过空槽
      if ((_slot_map & (1ull << i)) == 0) {
        continue;
      }
      // 取出该槽位的整条任务链表
      auto task_ptr = std::move(_task_slots[i]);
      _slot_map &= ~(1ull << i);

      // 遍历链表，逐个执行到期任务
      while (task_ptr != nullptr) {
        auto next = std::move(task_ptr->_next);
        task_ptr->execute(local_queue, global_queue);
        ++count;
        task_ptr = std::move(next);
      }
    }
  }

  /// 获取最近一个到期任务的时间（距离当前位置的偏移，单位 ms）
  /// @return 最近到期的槽位偏移；若无任务返回 SLOT_SIZE
  [[nodiscard]]
  auto next_deadline_time() const noexcept -> std::size_t {
    if (_slot_map == 0) {
      return SLOT_SIZE;
    }
    // 利用 countr_zero 找到 bitmap 中最低位为 1 的位置
    return static_cast<std::size_t>(std::countr_zero(_slot_map));
  }

  /// 旋转时间轮：移除前 start 个槽位，将后面的槽位前移
  /// 这个操作在父层级降级或推进时调用
  /// @param start 要跳过的起始位置
  void rotate(std::size_t start) {
    if (start == 0 || start >= SLOT_SIZE) {
      return;
    }
    // 将 bitmap 右移 start 位
    _slot_map >>= start;
    // 将槽位数组前移
    for (std::size_t i = 0; i < SLOT_SIZE - start; ++i) {
      _task_slots[i] = std::move(_task_slots[i + start]);
    }
    // 清空尾部多余的槽位
    for (std::size_t i = SLOT_SIZE - start; i < SLOT_SIZE; ++i) {
      _task_slots[i].reset();
    }
  }

  /// 判断当前时间轮是否为空
  [[nodiscard]]
  auto empty() const noexcept -> bool {
    return _slot_map == 0;
  }

  /// 获取槽位映射 bitmap
  [[nodiscard]]
  auto slot_map() const noexcept -> std::uint64_t {
    return _slot_map;
  }

  /// 升级到更高层级的时间轮
  /// 当前 level-0 轮变为 level-1 轮的第0号子轮
  /// @param me 当前时间轮的 unique_ptr（所有权转移）
  /// @return 新创建的父时间轮（level-1）
  [[nodiscard]]
  auto level_up(std::unique_ptr<TimerWheel> &&me) -> father_wheel_ptr {
    fastlog::console.debug("TimerWheel<0>::level_up: upgrading to level 1");
    return std::make_unique<father_wheel>(std::move(me));
  }

  /// level-0 无法降级，始终返回 false
  [[nodiscard]]
  auto can_level_down() const noexcept -> bool {
    return false;
  }

private:
  /// 任务槽位数组，每个槽位是一个 TimerTask 链表头
  std::array<std::unique_ptr<TimerTask>, SLOT_SIZE> _task_slots{};
  /// 位图：第 i 位为 1 表示 _task_slots[i] 非空
  std::uint64_t _slot_map{0};
};

} // namespace faio::runtime::detail::timer
#endif // FAIO_DETAIL_RUNTIME_CORE_TIMER_TIMERWHEEL_HPP
