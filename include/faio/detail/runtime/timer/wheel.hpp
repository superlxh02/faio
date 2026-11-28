/**
 * @file faio/detail/runtime/timer/wheel.hpp
 * @brief 多级时间轮。
 */
#ifndef FAIO_DETAIL_RUNTIME_TIMER_WHEEL_HPP
#define FAIO_DETAIL_RUNTIME_TIMER_WHEEL_HPP

#include "faio/detail/common/static_math.hpp"
#include "faio/detail/runtime/common/constants.hpp"
#include "faio/detail/runtime/timer/task.hpp"
#include "faio/detail/log/logger.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace faio::runtime::detail::timer {

/**
 * @brief 多级时间轮的通用模板，适用于 LEVEL >= 1。
 * 各层时间跨度如下：
 * LEVEL 0：底层，每槽 1 ms，覆盖 64 ms。
 * LEVEL 1：每槽覆盖 64 ms（一个 LEVEL-0 轮），总计 64*64 = 4096 ms，约 4 s。
 * LEVEL 2：每槽覆盖 4096 ms，总计 64^3 = 262144 ms，约 4.4 min。
 * LEVEL N：每槽覆盖 64^N ms，总计 64^(N+1) ms。
 * 每个非底层时间轮的槽位存储一个子时间轮指针。
 * 使用 bitmap (_slot_map) 加速非空槽位查找。
 */

template <std::size_t LEVEL>
class timer_wheel {
  static_assert(LEVEL >= 1, "LEVEL must be >= 1, use TimerWheel<0> specialization for level 0");

 public:
  using child_wheel = timer_wheel<LEVEL - 1>;           /**< 子时间轮类型 */
  using child_wheel_ptr = std::unique_ptr<child_wheel>; /**< 子时间轮指针 */

  static constexpr std::size_t CHILD_SPAN_MS =
      util::static_pow(SLOT_SIZE, LEVEL); /**< 子层级一个完整轮的时间跨度（ms），等于 64^LEVEL */
  static constexpr std::size_t SPAN_MS = util::static_pow(
      SLOT_SIZE, LEVEL + 1uz); /**< 当前层级整个时间轮的总跨度（ms），等于 64^(LEVEL+1) */
  static constexpr std::size_t CHILD_SHIFT =
      SLOT_SHIFT * LEVEL; /**< CHILD_SPAN_MS 的移位量：log2(CHILD_SPAN_MS) = SLOT_SHIFT * LEVEL */
  static constexpr std::size_t CHILD_MASK =
      CHILD_SPAN_MS - 1uz; /**< CHILD_SPAN_MS 的掩码：CHILD_SPAN_MS - 1 */

 public:
  /** @brief 构造分层时间轮。 */
  timer_wheel() = default;

  /**
   * @brief 由一个已有的子时间轮升级构造
   * @param child 要放入第0号槽位的子时间轮
   */
  explicit timer_wheel(child_wheel_ptr&& child) : _slot_map(1ull) {
    _wheels_slots[0] = std::move(child);
  }

  /** @brief 销毁分层时间轮。 */
  ~timer_wheel() = default;

  /** @brief 禁止复制构造分层时间轮。 */
  timer_wheel(const timer_wheel&) = delete;

  /** @brief 禁止复制赋值。 */
  timer_wheel& operator=(const timer_wheel&) = delete;

  /** @brief 移动构造分层时间轮。 */
  timer_wheel(timer_wheel&&) = default;

  /** @brief 移动赋值。 */
  timer_wheel& operator=(timer_wheel&&) = default;

 public:
  /**
   * @brief 添加定时器任务到合适的子时间轮槽位
   * @param task 待添加的定时器任务
   * @param interval 距当前位置的时间间隔（ms）
   */
  void add_task(std::unique_ptr<timer_task>&& task, std::size_t interval) {
    auto slot_idx = interval >> CHILD_SHIFT;
    if (slot_idx >= SLOT_SIZE) {
      faio::log::logger()->error(
          "TimerWheel<{}>::add_task: interval {} exceeds wheel span, "
          "slot_idx={}, clamping to last slot",
          LEVEL,
          interval,
          slot_idx);
      slot_idx = SLOT_SIZE - 1;
    }

    if (_wheels_slots[slot_idx] == nullptr) {
      _wheels_slots[slot_idx] = std::make_unique<child_wheel>();
      _slot_map |= (1ull << slot_idx);
    }

    auto child_interval = interval & CHILD_MASK;
    _wheels_slots[slot_idx]->add_task(std::move(task), child_interval);
  }

  /** @brief 只在所属 worker 的 timer poll 中清理取消项；非取消路径没有扫描成本。 */
  std::size_t prune_cancelled() {
    std::size_t removed = 0;
    for (std::size_t i = 0; i < SLOT_SIZE; ++i) {
      if (!_wheels_slots[i])
        continue;
      removed += _wheels_slots[i]->prune_cancelled();
      if (_wheels_slots[i]->empty()) {
        _wheels_slots[i].reset();
        _slot_map &= ~(1ull << i);
      }
    }
    return removed;
  }

  /**
   * @brief 从子时间轮中移除定时器任务
   * @param task 待移除的任务指针
   * @param interval 任务所在的时间间隔
   */
  void remove_task(timer_task* task, std::size_t interval) {
    auto slot_idx = interval >> CHILD_SHIFT;
    if (slot_idx >= SLOT_SIZE || _wheels_slots[slot_idx] == nullptr) {
      faio::log::logger()->warn(
          "TimerWheel<{}>::remove_task: invalid slot {} or empty child wheel", LEVEL, slot_idx);
      return;
    }

    auto child_interval = interval & CHILD_MASK;
    _wheels_slots[slot_idx]->remove_task(task, child_interval);

    if (_wheels_slots[slot_idx]->empty()) {
      _wheels_slots[slot_idx].reset();
      _slot_map &= ~(1ull << slot_idx);
    }
  }

  /**
   * @brief 递归处理子时间轮中的到期任务。
   * @param sink 所属线程的就绪接收端
   * @param count 已处理任务计数（累加）
   * @param remaining_ms 剩余需要处理的毫秒数
   */
  template <ready_sink sink_type>
  void handle_expired_tasks(sink_type& sink, std::size_t& count, std::size_t remaining_ms) {
    if (_slot_map == 0 || remaining_ms == 0) {
      return;
    }

    auto full_slots = remaining_ms >> CHILD_SHIFT;
    auto partial_remaining = remaining_ms & CHILD_MASK;

    for (std::size_t i = 0; i < full_slots && i < SLOT_SIZE; ++i) {
      if ((_slot_map & (1ull << i)) == 0) {
        continue;
      }
      _wheels_slots[i]->handle_expired_tasks(sink, count, child_wheel::SPAN_MS);
      _wheels_slots[i].reset();
      _slot_map &= ~(1ull << i);
    }

    if (partial_remaining > 0 && full_slots < SLOT_SIZE) {
      if ((_slot_map & (1ull << full_slots)) != 0) {
        _wheels_slots[full_slots]->handle_expired_tasks(sink, count, partial_remaining);

        if (_wheels_slots[full_slots]->empty()) {
          _wheels_slots[full_slots].reset();
          _slot_map &= ~(1ull << full_slots);
        }
      }
    }
  }

  /**
   * @brief 获取最近到期任务的时间偏移（ms）
   * @return 距当前位置最近的任务的时间偏移
   */
  [[nodiscard]]
  auto next_deadline_time() const noexcept -> std::size_t {
    if (_slot_map == 0) {
      return SPAN_MS;
    }
    auto first_slot = static_cast<std::size_t>(std::countr_zero(_slot_map));

    auto child_deadline = _wheels_slots[first_slot]->next_deadline_time();
    return (first_slot << CHILD_SHIFT) + child_deadline;
  }

  /**
   * @brief 旋转时间轮：推进 start 个子时间轮的距离
   * 前 start 个槽位被丢弃，后面的前移
   * @param start 要推进的槽位数
   */
  void rotate(std::size_t start) {
    if (start == 0 || start >= SLOT_SIZE) {
      return;
    }
    _slot_map >>= start;
    for (std::size_t i = 0; i < SLOT_SIZE - start; ++i) {
      _wheels_slots[i] = std::move(_wheels_slots[i + start]);
    }
    for (std::size_t i = SLOT_SIZE - start; i < SLOT_SIZE; ++i) {
      _wheels_slots[i].reset();
    }
  }

  /**
   * @brief 升级到更高层级的时间轮
   * 当前时间轮变为父时间轮第0个槽位的子轮
   * 仅当 LEVEL < MAX_LEVEL 时可用（最高层无法再升级）
   * @param me 当前时间轮的 unique_ptr（所有权转移）
   * @return 新创建的父时间轮
   */
  [[nodiscard]]
  auto level_up(std::unique_ptr<timer_wheel>&& me) -> std::unique_ptr<timer_wheel<LEVEL + 1>>
    requires(LEVEL < MAX_LEVEL)
  {
    faio::log::logger()->trace("TimerWheel<{}>::level_up: upgrading to level {}", LEVEL, LEVEL + 1);
    return std::make_unique<timer_wheel<LEVEL + 1>>(std::move(me));
  }

  /**
   * @brief 降级到子时间轮
   * 取出第0号槽位的子时间轮并返回
   * @return 第0号子时间轮的 unique_ptr
   */
  [[nodiscard]]
  auto level_down() -> child_wheel_ptr {
    if (_wheels_slots[0] == nullptr) {
      return nullptr;
    }
    auto child = std::move(_wheels_slots[0]);
    _slot_map &= ~1ull;
    faio::log::logger()->trace(
        "TimerWheel<{}>::level_down: downgrading to level {}", LEVEL, LEVEL - 1);
    return child;
  }

  /**
   * @brief 判断是否可以降级
   * 条件：只有第0个槽位非空（或整个轮为空）
   */
  [[nodiscard]]
  auto can_level_down() const noexcept -> bool {
    return (_slot_map & ~1ull) == 0;
  }

  /** @brief 判断当前时间轮是否为空 */
  [[nodiscard]]
  auto empty() const noexcept -> bool {
    return _slot_map == 0;
  }

  /** @brief 取得槽位位图。 */
  [[nodiscard]]
  auto slot_map() const noexcept -> std::uint64_t {
    return _slot_map;
  }

 private:
  std::array<child_wheel_ptr, SLOT_SIZE> _wheels_slots{}; /**< 子时间轮槽位数组 */
  std::uint64_t _slot_map{0}; /**< 位图：第 i 位为 1 表示 _wheels_slots[i] 非空 */
};

/**
 * @brief 最底层时间轮的 TimerWheel<0> 特化。
 * 每个槽位存储一个 TimerTask 单链表，直接承载定时器任务。
 * 共 SLOT_SIZE = 64 个槽位，每个槽位对应 1 ms。
 */

template <>
class timer_wheel<0uz> {
 public:
  using father_wheel = timer_wheel<1uz>;                  /**< 父时间轮类型 */
  using father_wheel_ptr = std::unique_ptr<father_wheel>; /**< 父时间轮指针 */

  static constexpr std::size_t SPAN_MS =
      SLOT_SIZE; /**< 最底层时间轮的跨度，共 SLOT_SIZE 个槽，每槽 1 ms，总计 64 ms。 */

 public:
  /** @brief 构造分层时间轮。 */
  timer_wheel() = default;

  /** @brief 销毁分层时间轮。 */
  ~timer_wheel() = default;

  /** @brief 禁止复制构造分层时间轮。 */
  timer_wheel(const timer_wheel&) = delete;

  /** @brief 禁止复制赋值。 */
  timer_wheel& operator=(const timer_wheel&) = delete;

  /** @brief 移动构造分层时间轮。 */
  timer_wheel(timer_wheel&&) = default;

  /** @brief 移动赋值。 */
  timer_wheel& operator=(timer_wheel&&) = default;

 public:
  /**
   * @brief 向指定时间间隔的槽位添加定时器任务
   * @param task 待添加的定时器任务（unique_ptr，所有权转移）
   * @param interval 从当前位置开始的时间间隔（ms），必须 < SLOT_SIZE
   */
  void add_task(std::unique_ptr<timer_task>&& task, std::size_t interval) {
    auto slot_idx = interval & SLOT_MASK;
    task->_next = std::move(_task_slots[slot_idx]);
    _task_slots[slot_idx] = std::move(task);
    _slot_map |= (1ull << slot_idx);
  }

  /** @brief 回收已取消的定时登记。 */
  std::size_t prune_cancelled() {
    std::size_t removed = 0;
    for (std::size_t i = 0; i < SLOT_SIZE; ++i) {
      auto* link = &_task_slots[i];
      while (*link) {
        if ((*link)->cancelled()) {
          *link = std::move((*link)->_next);
          ++removed;
        } else {
          link = &(*link)->_next;
        }
      }
      if (!_task_slots[i])
        _slot_map &= ~(1ull << i);
    }
    return removed;
  }

  /**
   * @brief 从指定槽位中移除定时器任务
   * @param task 待移除的任务指针（裸指针，用于查找匹配）
   * @param interval 任务所在的时间间隔
   */
  void remove_task(timer_task* task, std::size_t interval) {
    auto slot_idx = interval & SLOT_MASK;
    timer_task* prev = nullptr;
    timer_task* current = _task_slots[slot_idx].get();

    while (current != nullptr) {
      if (current == task) {
        if (prev == nullptr) {
          _task_slots[slot_idx] = std::move(current->_next);
        } else {
          prev->_next = std::move(current->_next);
        }
        if (_task_slots[slot_idx] == nullptr) {
          _slot_map &= ~(1ull << slot_idx);
        }
        return;
      }
      prev = current;
      current = current->_next.get();
    }
    faio::log::logger()->warn(
        "TimerWheel<0>::remove_task: task not found in "
        "slot {}",
        slot_idx);
  }

  /**
   * @brief 处理到期任务：扫描 [0, remaining_ms) 范围内所有槽位，执行到期任务
   * @param sink 所属线程的就绪接收端，不依赖具体队列
   * @param count 已处理任务计数器（输出参数，累加）
   * @param remaining_ms 剩余需要处理的毫秒数
   */
  template <ready_sink sink_type>
  void handle_expired_tasks(sink_type& sink, std::size_t& count, std::size_t remaining_ms) {
    auto slots_to_scan = std::min(remaining_ms, SLOT_SIZE);

    for (std::size_t i = 0; i < slots_to_scan; ++i) {
      if ((_slot_map & (1ull << i)) == 0) {
        continue;
      }
      auto task_ptr = std::move(_task_slots[i]);
      _slot_map &= ~(1ull << i);

      while (task_ptr != nullptr) {
        auto next = std::move(task_ptr->_next);
        task_ptr->execute(sink);
        ++count;
        task_ptr = std::move(next);
      }
    }
  }

  /**
   * @brief 取得最近到期任务相对于当前位置的时间偏移，单位为 ms。
   * @return 最近到期的槽位偏移；若无任务返回 SLOT_SIZE
   */
  [[nodiscard]]
  auto next_deadline_time() const noexcept -> std::size_t {
    if (_slot_map == 0) {
      return SLOT_SIZE;
    }
    return static_cast<std::size_t>(std::countr_zero(_slot_map));
  }

  /**
   * @brief 旋转时间轮：移除前 start 个槽位，将后面的槽位前移
   * 这个操作在父层级降级或推进时调用
   * @param start 要跳过的起始位置
   */
  void rotate(std::size_t start) {
    if (start == 0 || start >= SLOT_SIZE) {
      return;
    }
    _slot_map >>= start;
    for (std::size_t i = 0; i < SLOT_SIZE - start; ++i) {
      _task_slots[i] = std::move(_task_slots[i + start]);
    }
    for (std::size_t i = SLOT_SIZE - start; i < SLOT_SIZE; ++i) {
      _task_slots[i].reset();
    }
  }

  /** @brief 判断当前时间轮是否为空 */
  [[nodiscard]]
  auto empty() const noexcept -> bool {
    return _slot_map == 0;
  }

  /** @brief 取得槽位位图。 */
  [[nodiscard]]
  auto slot_map() const noexcept -> std::uint64_t {
    return _slot_map;
  }

  /**
   * @brief 升级到更高层级的时间轮
   * 当前 level-0 轮变为 level-1 轮的第0号子轮
   * @param me 当前时间轮的 unique_ptr（所有权转移）
   * @return 新创建的父时间轮（level-1）
   */
  [[nodiscard]]
  auto level_up(std::unique_ptr<timer_wheel>&& me) -> father_wheel_ptr {
    faio::log::logger()->trace("TimerWheel<0>::level_up: upgrading to level 1");
    return std::make_unique<father_wheel>(std::move(me));
  }

  /** @brief level-0 无法降级，始终返回 false */
  [[nodiscard]]
  auto can_level_down() const noexcept -> bool {
    return false;
  }

 private:
  std::array<std::unique_ptr<timer_task>, SLOT_SIZE>
      _task_slots{};          /**< 任务槽位数组，每个槽位是一个 TimerTask 链表头 */
  std::uint64_t _slot_map{0}; /**< 位图：第 i 位为 1 表示 _task_slots[i] 非空 */
};
}  // namespace faio::runtime::detail::timer
#endif
