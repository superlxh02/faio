/**
 * @file faio/detail/runtime/timer/wheel.hpp
 * @brief 运行时调度、线程协调与定时驱动：多级时间轮。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#ifndef FAIO_DETAIL_RUNTIME_TIMER_WHEEL_HPP
#define FAIO_DETAIL_RUNTIME_TIMER_WHEEL_HPP

#include "faio/detail/common/static_math.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/timer/task.hpp"
#include "faio/log.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace faio::runtime::detail::timer {

/// @brief TimerWheel<LEVEL> 通用模板 —— 多级时间轮（LEVEL >= 1）
///
/// 层级结构：
/// LEVEL 0: 底层，每槽 1ms，范围 64ms
/// LEVEL 1: 每槽管辖 64ms（一个 LEVEL-0 轮），范围 64*64 = 4096ms ≈ 4s
/// LEVEL 2: 每槽管辖 4096ms，范围 64^3 = 262144ms ≈ 4.4min
/// LEVEL N: 每槽管辖 64^N ms，范围 64^(N+1) ms
///
/// 每个非底层时间轮的槽位存储一个子时间轮指针。
/// 使用 bitmap (_slot_map) 加速非空槽位查找。

template <std::size_t LEVEL>
class TimerWheel {
  static_assert(LEVEL >= 1, "LEVEL must be >= 1, use TimerWheel<0> specialization for level 0");

 public:
  using child_wheel = TimerWheel<LEVEL - 1>;  ///< 子时间轮类型
  using child_wheel_ptr = std::unique_ptr<child_wheel>;  ///< 子时间轮指针

  static constexpr std::size_t CHILD_SPAN_MS = util::static_pow(SLOT_SIZE, LEVEL);  ///< 子层级一个完整轮的时间跨度（ms），等于 64^LEVEL
  static constexpr std::size_t SPAN_MS = util::static_pow(SLOT_SIZE, LEVEL + 1uz);  ///< 当前层级整个时间轮的总跨度（ms），等于 64^(LEVEL+1)
  static constexpr std::size_t CHILD_SHIFT = SLOT_SHIFT * LEVEL;  ///< CHILD_SPAN_MS 的移位量：log2(CHILD_SPAN_MS) = SLOT_SHIFT * LEVEL
  static constexpr std::size_t CHILD_MASK = CHILD_SPAN_MS - 1uz;  ///< CHILD_SPAN_MS 的掩码：CHILD_SPAN_MS - 1

 public:
  TimerWheel() = default;

  /// @brief 由一个已有的子时间轮升级构造
  /// @param child 要放入第0号槽位的子时间轮
  explicit TimerWheel(child_wheel_ptr&& child) : _slot_map(1ull) {
    /// @brief 将原根轮接管为第零槽子轮，升级后仍保留原来的时间坐标起点。
    _wheels_slots[0] = std::move(child);
  }

  ~TimerWheel() = default;

  TimerWheel(const TimerWheel&) = delete;

  TimerWheel& operator=(const TimerWheel&) = delete;

  TimerWheel(TimerWheel&&) = default;

  TimerWheel& operator=(TimerWheel&&) = default;

 public:
  /// @brief 添加定时器任务到合适的子时间轮槽位
  /// @param task 待添加的定时器任务
  /// @param interval 距当前位置的时间间隔（ms）
  void add_task(std::unique_ptr<TimerTask>&& task, std::size_t interval) {
    /// @brief 计算任务应该落在哪个槽位（右移代替除法）
    auto slot_idx = interval >> CHILD_SHIFT;
    /// @brief 越过当前层可表达范围时记录错误，并将索引限制到最后一槽。
    if (slot_idx >= SLOT_SIZE) {
      /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
      faio::log::logger()->error(
          "TimerWheel<{}>::add_task: interval {} exceeds wheel span, "
          "slot_idx={}, clamping to last slot",
          LEVEL,
          interval,
          slot_idx);
      /// @brief 限制为有效槽索引，防止越界访问子轮数组。
      slot_idx = SLOT_SIZE - 1;
    }

    /// @brief 如果该槽位的子时间轮尚未创建，则懒创建
    if (_wheels_slots[slot_idx] == nullptr) {
      /// @brief 仅为实际有任务的区间分配子时间轮，空区间不预建整棵轮树。
      _wheels_slots[slot_idx] = std::make_unique<child_wheel>();
      /// @brief 将当前槽标记为非空，使后续到期查询与扫描能够发现该子轮。
      _slot_map |= (1ull << slot_idx);
    }

    /// @brief 计算在子时间轮内的偏移量（掩码代替取余）
    auto child_interval = interval & CHILD_MASK;
    /// @brief 将任务所有权及槽内相对偏移递归交给下一层时间轮。
    _wheels_slots[slot_idx]->add_task(std::move(task), child_interval);
  }

  /// @brief 只在所属 worker 的 timer poll 中清理取消项；非取消路径没有扫描成本。
  std::size_t prune_cancelled() {
    /// @brief 累计本次取消清理实际摘除的任务数，调用方据此修正活跃计数。
    std::size_t removed = 0;
    /// @brief 逐槽检查本层已分配存储；此扫描仅发生在已有取消请求时。
    for (std::size_t i = 0; i < SLOT_SIZE; ++i) {
      /// @brief 未分配的子轮没有任务，跳过递归清理。
      if (!_wheels_slots[i])
        continue;
      /// @brief 让子轮移除已被取消路径领取的节点，并累加真实移除数量。
      removed += _wheels_slots[i]->prune_cancelled();
      /// @brief 子轮清理后为空时可立即释放其存储，并撤销位图中的存在标记。
      if (_wheels_slots[i]->empty()) {
        /// @brief 释放已经排空或被旋转丢弃的子轮，不再保留该区间的节点所有权。
        _wheels_slots[i].reset();
        /// @brief 清除该槽的非空标记，保持位图与实际拥有的节点一致。
        _slot_map &= ~(1ull << i);
      }
    }
    /// @brief 把真实移除数量交给上层，取消过但尚未回收的任务不重复计数。
    return removed;
  }

  /// @brief 从子时间轮中移除定时器任务
  /// @param task 待移除的任务指针
  /// @param interval 任务所在的时间间隔
  void remove_task(TimerTask* task, std::size_t interval) {
    /// @brief 用子轮跨度对应的位数右移，定位任务所在的本层槽位。
    auto slot_idx = interval >> CHILD_SHIFT;
    /// @brief 拒绝超出本层范围或根本没有子轮的移除请求，避免无效解引用。
    if (slot_idx >= SLOT_SIZE || _wheels_slots[slot_idx] == nullptr) {
      /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
      faio::log::logger()->warn(
          "TimerWheel<{}>::remove_task: invalid slot {} or empty child wheel", LEVEL, slot_idx);
      return;
    }

    /// @brief 只保留子轮范围内的低位偏移，递归操作继续使用同一起点坐标系。
    auto child_interval = interval & CHILD_MASK;
    /// @brief 按相对偏移递归查找目标节点；裸指针只用于身份比较，不转移所有权。
    _wheels_slots[slot_idx]->remove_task(task, child_interval);

    /// @brief 如果子时间轮变空了，释放并清除 bitmap 标记
    if (_wheels_slots[slot_idx]->empty()) {
      /// @brief 子轮已无任务，回收子轮并让该时间区间重新成为未分配状态。
      _wheels_slots[slot_idx].reset();
      /// @brief 同步撤销对应位，避免最近截止时间查询访问已释放子轮。
      _slot_map &= ~(1ull << slot_idx);
    }
  }

  /// @brief 处理到期任务：递归处理子时间轮中的到期任务
  /// @param sink 所属线程的就绪接收端
  /// @param count 已处理任务计数（累加）
  /// @param remaining_ms 剩余需要处理的毫秒数
  template <ready_sink sink_type>
  void handle_expired_tasks(sink_type& sink, std::size_t& count, std::size_t remaining_ms) {
    /// @brief 没有节点或没有可推进的完整毫秒时无需进入递归到期扫描。
    if (_slot_map == 0 || remaining_ms == 0) {
      return;
    }

    /// @brief 计算需要完整扫描的槽位数（移位代替除法，掩码代替取余）
    auto full_slots = remaining_ms >> CHILD_SHIFT;
    /// @brief 余下低位表示最后一个子轮内已到期的前缀，不能把整轮都清空。
    auto partial_remaining = remaining_ms & CHILD_MASK;

    /// @brief 处理需要完整清空的槽位
    for (std::size_t i = 0; i < full_slots && i < SLOT_SIZE; ++i) {
      /// @brief 位图证明该槽为空时直接跳过，避免解引用未分配的存储。
      if ((_slot_map & (1ull << i)) == 0) {
        continue;
      }
      /// @brief 完整处理整个子时间轮
      _wheels_slots[i]->handle_expired_tasks(sink, count, child_wheel::SPAN_MS);
      /// @brief 释放已经排空或被旋转丢弃的子轮，不再保留该区间的节点所有权。
      _wheels_slots[i].reset();
      /// @brief 清除该槽的非空标记，保持位图与实际拥有的节点一致。
      _slot_map &= ~(1ull << i);
    }

    /// @brief 处理部分到期的槽位（第 full_slots 个槽位可能只有部分到期）
    if (partial_remaining > 0 && full_slots < SLOT_SIZE) {
      /// @brief 仅当最后一个部分覆盖的槽实际非空时才递归扫描其前缀。
      if ((_slot_map & (1ull << full_slots)) != 0) {
        /// @brief 只处理该子轮已到期的毫秒前缀，保留仍未到期任务及原内部坐标。
        _wheels_slots[full_slots]->handle_expired_tasks(sink, count, partial_remaining);

        /// @brief 如果子时间轮处理后变空，清理
        if (_wheels_slots[full_slots]->empty()) {
          /// @brief 部分扫描后子轮已空，释放其存储。
          _wheels_slots[full_slots].reset();
          /// @brief 清除刚释放子轮对应的非空位。
          _slot_map &= ~(1ull << full_slots);
        }
      }
    }
  }

  /// @brief 获取最近到期任务的时间偏移（ms）
  /// @return 距当前位置最近的任务的时间偏移
  [[nodiscard]]
  auto next_deadline_time() const noexcept -> std::size_t {
    /// @brief 空位图表示整层没有任务，最近截止时间以本层跨度作为哨兵。
    if (_slot_map == 0) {
      /// @brief 返回本层完整跨度，表示该轮范围内没有更早任务。
      return SPAN_MS;
    }
    /// @brief 找到最低非空槽位
    auto first_slot = static_cast<std::size_t>(std::countr_zero(_slot_map));

    /// @brief 递归查询子时间轮中最近到期的时间
    auto child_deadline = _wheels_slots[first_slot]->next_deadline_time();
    /// @brief 将子轮内的最近偏移加上槽起点，还原为相对当前轮起点的毫秒偏移。
    return (first_slot << CHILD_SHIFT) + child_deadline;
  }

  /// @brief 旋转时间轮：推进 start 个子时间轮的距离
  /// 前 start 个槽位被丢弃，后面的前移
  /// @param start 要推进的槽位数
  void rotate(std::size_t start) {
    /// @brief 只处理非零且小于一整轮的旋转量；无效跨度不改变当前节点坐标。
    if (start == 0 || start >= SLOT_SIZE) {
      return;
    }
    /// @brief 位图右移
    _slot_map >>= start;
    /// @brief 槽位数组前移
    for (std::size_t i = 0; i < SLOT_SIZE - start; ++i) {
      /// @brief 把尚未丢弃区间的子轮所有权前移，原槽在移动后不再拥有该子轮。
      _wheels_slots[i] = std::move(_wheels_slots[i + start]);
    }
    /// @brief 清空尾部
    for (std::size_t i = SLOT_SIZE - start; i < SLOT_SIZE; ++i) {
      /// @brief 释放已经排空或被旋转丢弃的子轮，不再保留该区间的节点所有权。
      _wheels_slots[i].reset();
    }
  }

  /// @brief 升级到更高层级的时间轮
  /// 当前时间轮变为父时间轮第0个槽位的子轮
  /// 仅当 LEVEL < MAX_LEVEL 时可用（最高层无法再升级）
  /// @param me 当前时间轮的 unique_ptr（所有权转移）
  /// @return 新创建的父时间轮
  [[nodiscard]]
  auto level_up(std::unique_ptr<TimerWheel>&& me) -> std::unique_ptr<TimerWheel<LEVEL + 1>>
    requires(LEVEL < MAX_LEVEL)
  {
    /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
    faio::log::logger()->trace("TimerWheel<{}>::level_up: upgrading to level {}", LEVEL, LEVEL + 1);
    /// @brief 建立上一层根轮，并把当前轮放入其第零槽；整个现存子树随所有权转移。
    return std::make_unique<TimerWheel<LEVEL + 1>>(std::move(me));
  }

  /// @brief 降级到子时间轮
  /// 取出第0号槽位的子时间轮并返回
  /// @return 第0号子时间轮的 unique_ptr
  [[nodiscard]]
  auto level_down() -> child_wheel_ptr {
    /// @brief 第零槽没有子轮时无法取出可降级的新根。
    if (_wheels_slots[0] == nullptr) {
      return nullptr;
    }
    /// @brief 将第零槽子轮所有权移出，降级前后其内部时间坐标不变。
    auto child = std::move(_wheels_slots[0]);
    /// @brief 第零槽已经移出，清除本层对它的非空记录。
    _slot_map &= ~1ull;
    /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
    faio::log::logger()->trace(
        "TimerWheel<{}>::level_down: downgrading to level {}", LEVEL, LEVEL - 1);
    /// @brief 把子轮所有权转交给调用方作为新的根轮。
    return child;
  }

  /// @brief 判断是否可以降级
  /// 条件：只有第0个槽位非空（或整个轮为空）
  [[nodiscard]]
  auto can_level_down() const noexcept -> bool {
    /// @brief 只有第0个槽位可能非空，其余槽位必须全空
    return (_slot_map & ~1ull) == 0;
  }

  /// @brief 判断当前时间轮是否为空
  [[nodiscard]]
  auto empty() const noexcept -> bool {
    /// @brief 根据与节点存储同步维护的位图判断整轮是否为空。
    return _slot_map == 0;
  }

  /// @brief 获取槽位映射 bitmap
  [[nodiscard]]
  auto slot_map() const noexcept -> std::uint64_t {
    /// @brief 返回非空槽位快照，供层级调整与诊断使用。
    return _slot_map;
  }

 private:
  std::array<child_wheel_ptr, SLOT_SIZE> _wheels_slots{};  ///< 子时间轮槽位数组
  std::uint64_t _slot_map{0};  ///< 位图：第 i 位为 1 表示 _wheels_slots[i] 非空
};

/// @brief TimerWheel<0> 特化 —— 最底层时间轮（叶子层）
/// 每个槽位存储一个 TimerTask 单链表，直接承载定时器任务。
/// SLOT_SIZE = 64 个槽位，每个槽位代表 1ms 的时间粒度。

template <>
class TimerWheel<0uz> {
 public:
  using father_wheel = TimerWheel<1uz>;  ///< 父时间轮类型
  using father_wheel_ptr = std::unique_ptr<father_wheel>;  ///< 父时间轮指针

  static constexpr std::size_t SPAN_MS = SLOT_SIZE;  ///< 最底层时间轮的时间跨度（ms），每个槽 1ms，共 SLOT_SIZE 个槽 时间跨度为 64 毫秒。

 public:
  TimerWheel() = default;

  ~TimerWheel() = default;

  TimerWheel(const TimerWheel&) = delete;

  TimerWheel& operator=(const TimerWheel&) = delete;

  TimerWheel(TimerWheel&&) = default;

  TimerWheel& operator=(TimerWheel&&) = default;

 public:
  /// @brief 向指定时间间隔的槽位添加定时器任务
  /// @param task 待添加的定时器任务（unique_ptr，所有权转移）
  /// @param interval 从当前位置开始的时间间隔（ms），必须 < SLOT_SIZE
  void add_task(std::unique_ptr<TimerTask>&& task, std::size_t interval) {
    /// @brief 叶子层每槽一毫秒，以掩码定位任务对应的叶子槽。
    auto slot_idx = interval & SLOT_MASK;
    /// @brief 将任务链入该槽位的链表头部
    task->_next = std::move(_task_slots[slot_idx]);
    /// @brief 让当前槽头接管新任务，原任务链已移动到新节点的后继。
    _task_slots[slot_idx] = std::move(task);
    /// @brief 在 bitmap 中标记该槽位非空
    _slot_map |= (1ull << slot_idx);
  }

  std::size_t prune_cancelled() {
    /// @brief 累计本次取消清理实际摘除的任务数，调用方据此修正活跃计数。
    std::size_t removed = 0;
    /// @brief 逐槽检查本层已分配存储；此扫描仅发生在已有取消请求时。
    for (std::size_t i = 0; i < SLOT_SIZE; ++i) {
      /// @brief 以拥有链接的地址遍历链表，删除头节点与中间节点可使用同一操作。
      auto* link = &_task_slots[i];
      /// @brief 只遍历当前槽仍然拥有的节点，取消节点会在原链接处被替换。
      while (*link) {
        /// @brief 已被取消路径领取恢复权的任务不再需要保留到原截止时间。
        if ((*link)->cancelled()) {
          /// @brief 接管后继以摘除并销毁当前取消节点，继续从同一链接检查下一节点。
          *link = std::move((*link)->_next);
          /// @brief 只为真正摘除并销毁的取消节点累计清理数，不能按取消请求次数减少活跃条目。
          ++removed;
        } else {
          /// @brief 未取消节点继续保留，推进到它拥有的下一个链接。
          link = &(*link)->_next;
        }
      }
      /// @brief 最后一个节点被清理后撤销该槽的非空提示。
      if (!_task_slots[i])
        /// @brief 清除该槽的非空标记，保持位图与实际拥有的节点一致。
        _slot_map &= ~(1ull << i);
    }
    /// @brief 把真实移除数量交给上层，取消过但尚未回收的任务不重复计数。
    return removed;
  }

  /// @brief 从指定槽位中移除定时器任务
  /// @param task 待移除的任务指针（裸指针，用于查找匹配）
  /// @param interval 任务所在的时间间隔
  void remove_task(TimerTask* task, std::size_t interval) {
    /// @brief 叶子层每槽一毫秒，以掩码定位任务对应的叶子槽。
    auto slot_idx = interval & SLOT_MASK;
    /// @brief 保存当前节点前驱；空值表示仍位于链表头。
    TimerTask* prev = nullptr;
    /// @brief 借用目标槽的链头用于身份查找，不接管节点所有权。
    TimerTask* current = _task_slots[slot_idx].get();

    /// @brief 逐个比较稳定节点地址，直到找到目标或到达链尾。
    while (current != nullptr) {
      /// @brief 地址相同即命中本次移除目标，随后修复唯一拥有链接。
      if (current == task) {
        /// @brief 目标为链头时由槽位接管后继；其余位置则由前驱接管后继。
        if (prev == nullptr) {
          /// @brief 移除链表头部节点
          _task_slots[slot_idx] = std::move(current->_next);
        } else {
          /// @brief 移除中间或尾部节点
          prev->_next = std::move(current->_next);
        }
        /// @brief 如果该槽位链表变空，清除 bitmap 标记
        if (_task_slots[slot_idx] == nullptr) {
          /// @brief 同步撤销对应位，避免最近截止时间查询访问已释放子轮。
          _slot_map &= ~(1ull << slot_idx);
        }
        return;
      }
      /// @brief 当前节点未命中时保存为下一轮前驱。
      prev = current;
      /// @brief 沿拥有链借用下一个节点，查找本身不移动节点。
      current = current->_next.get();
    }
    /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
    faio::log::logger()->warn(
        "TimerWheel<0>::remove_task: task not found in "
        "slot {}",
        slot_idx);
  }

  /// @brief 处理到期任务：扫描 [0, remaining_ms) 范围内所有槽位，执行到期任务
  /// @param sink 所属线程的就绪接收端，不依赖具体队列
  /// @param count 已处理任务计数器（输出参数，累加）
  /// @param remaining_ms 剩余需要处理的毫秒数
  template <ready_sink sink_type>
  void handle_expired_tasks(sink_type& sink, std::size_t& count, std::size_t remaining_ms) {
    /// @brief 计算实际需要扫描的槽位数
    auto slots_to_scan = std::min(remaining_ms, SLOT_SIZE);

    /// @brief 扫描本轮已经到期的叶子槽，超过本轮容量的毫秒不会造成数组越界。
    for (std::size_t i = 0; i < slots_to_scan; ++i) {
      /// @brief 利用 bitmap 快速跳过空槽
      if ((_slot_map & (1ull << i)) == 0) {
        continue;
      }
      /// @brief 取出该槽位的整条任务链表
      auto task_ptr = std::move(_task_slots[i]);
      /// @brief 清除该槽的非空标记，保持位图与实际拥有的节点一致。
      _slot_map &= ~(1ull << i);

      /// @brief 遍历链表，逐个执行到期任务
      while (task_ptr != nullptr) {
        /// @brief 执行前先移出后继，使当前节点完成后可以安全释放而不破坏剩余链。
        auto next = std::move(task_ptr->_next);
        /// @brief 让到期节点竞争唯一恢复权，并把胜出的协程句柄交给就绪接收端。
        task_ptr->execute(sink);
        /// @brief 累计从时间轮取出的到期节点数量，上层据此归还活跃条目计数。
        ++count;
        /// @brief 释放已处理节点并继续独占尚未处理的后继链。
        task_ptr = std::move(next);
      }
    }
  }

  /// @brief 获取最近一个到期任务的时间（距离当前位置的偏移，单位 ms）
  /// @return 最近到期的槽位偏移；若无任务返回 SLOT_SIZE
  [[nodiscard]]
  auto next_deadline_time() const noexcept -> std::size_t {
    /// @brief 空位图表示整层没有任务，最近截止时间以本层跨度作为哨兵。
    if (_slot_map == 0) {
      /// @brief 空叶子轮没有更早截止时间，以完整叶子跨度作为查询哨兵。
      return SLOT_SIZE;
    }
    /// @brief 利用 countr_zero 找到 bitmap 中最低位为 1 的位置
    return static_cast<std::size_t>(std::countr_zero(_slot_map));
  }

  /// @brief 旋转时间轮：移除前 start 个槽位，将后面的槽位前移
  /// 这个操作在父层级降级或推进时调用
  /// @param start 要跳过的起始位置
  void rotate(std::size_t start) {
    /// @brief 只处理非零且小于一整轮的旋转量；无效跨度不改变当前节点坐标。
    if (start == 0 || start >= SLOT_SIZE) {
      return;
    }
    /// @brief 将 bitmap 右移 start 位
    _slot_map >>= start;
    /// @brief 将槽位数组前移
    for (std::size_t i = 0; i < SLOT_SIZE - start; ++i) {
      /// @brief 前移剩余任务链的唯一所有权，使后续槽索引对应新的根起点。
      _task_slots[i] = std::move(_task_slots[i + start]);
    }
    /// @brief 清空尾部多余的槽位
    for (std::size_t i = SLOT_SIZE - start; i < SLOT_SIZE; ++i) {
      /// @brief 清空旋转后尾部不再对应有效区间的任务链头。
      _task_slots[i].reset();
    }
  }

  /// @brief 判断当前时间轮是否为空
  [[nodiscard]]
  auto empty() const noexcept -> bool {
    /// @brief 根据与节点存储同步维护的位图判断整轮是否为空。
    return _slot_map == 0;
  }

  /// @brief 获取槽位映射 bitmap
  [[nodiscard]]
  auto slot_map() const noexcept -> std::uint64_t {
    /// @brief 返回非空槽位快照，供层级调整与诊断使用。
    return _slot_map;
  }

  /// @brief 升级到更高层级的时间轮
  /// 当前 level-0 轮变为 level-1 轮的第0号子轮
  /// @param me 当前时间轮的 unique_ptr（所有权转移）
  /// @return 新创建的父时间轮（level-1）
  [[nodiscard]]
  auto level_up(std::unique_ptr<TimerWheel>&& me) -> father_wheel_ptr {
    /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
    faio::log::logger()->trace("TimerWheel<0>::level_up: upgrading to level 1");
    /// @brief 把叶子轮移动到新父轮的第零槽，保留原任务相对起点的位置。
    return std::make_unique<father_wheel>(std::move(me));
  }

  /// @brief level-0 无法降级，始终返回 false
  [[nodiscard]]
  auto can_level_down() const noexcept -> bool {
    return false;
  }

 private:
  std::array<std::unique_ptr<TimerTask>, SLOT_SIZE> _task_slots{};  ///< 任务槽位数组，每个槽位是一个 TimerTask 链表头
  std::uint64_t _slot_map{0};  ///< 位图：第 i 位为 1 表示 _task_slots[i] 非空
};
}
#endif
