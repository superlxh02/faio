/**
 * @file faio/detail/runtime/timer/timer.hpp
 * @brief 运行时调度、线程协调与定时驱动：时间轮管理与层级调整。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
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

/// @brief VariantWheelBuilder：利用 std::variant 构造能存放任意层级时间轮的类型
/// 生成 std::variant<std::monostate, unique_ptr<TimerWheel<0>>,
/// 后续类型为 unique_ptr<TimerWheel<1>>, ...,
/// 最后一个类型为 unique_ptr<TimerWheel<N-1>>>

template <std::size_t... N>
static inline constexpr auto variant_wheel_impl(std::index_sequence<N...>) {
  return std::variant<std::monostate, std::unique_ptr<TimerWheel<N>>...>{};
}

template <std::size_t N>
struct VariantWheelBuilder {
  using Type = decltype(variant_wheel_impl(std::make_index_sequence<N>{}));  ///< 容纳所有时间轮层级的变体类型。
};

/// @brief Timer 类 —— 对外暴露的定时器管理器
///
/// 核心设计：
/// - 每个 worker 线程拥有一个独立的 Timer 实例（通过 thread_local 指针）
/// - 内部用 variant 存放不同层级的根时间轮，实现动态升降级
/// - _start 始终对应根轮第 0 个槽位的起点，不能任意改为当前时间
/// - 插入、删除、轮询都使用 deadline - _start 这一坐标系
/// - add_task / remove_task / poll 为主要操作接口

class Timer;
inline thread_local Timer* current_timer;

class Timer {
 public:
  /// @brief stop_callback 可从其他线程调用；只记录请求，时间轮本身仍由所属 worker
  /// 修改。
  void request_prune() noexcept { _cancel_requests.fetch_add(1, std::memory_order_release); }

  Timer(const Timer&) = delete;

  Timer& operator=(const Timer&) = delete;

  Timer(Timer&&) = delete;

  Timer& operator=(Timer&&) = delete;

  explicit Timer(bool bind_thread = true) {
    /// @brief 按构造选项决定是否把管理器安装为当前线程默认定时服务。
    if (bind_thread)
      /// @brief 发布线程私有定时器指针，后续休眠等待器据此登记到所属轮。
      current_timer = this;
    /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
    faio::log::logger()->debug("Timer: initialized at thread");
  }

  ~Timer() {
    /// @brief 仅当线程默认指针仍指向本对象时才撤销绑定，避免清除后装入的管理器。
    if (current_timer == this)
      /// @brief 清除线程借用入口，防止管理器析构后留下悬空定时器指针。
      current_timer = nullptr;
    /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
    faio::log::logger()->debug("Timer: destroyed, entries remaining={}", _num_entries);
  }

 public:
  /// @brief 添加定时器任务
  /// @param deadline 任务的绝对到期时间
  /// @param handle 任务到期时要恢复的协程句柄
  /// @return 任务的裸指针（用于后续移除操作），调用者不拥有所有权
  auto add_task(std::chrono::steady_clock::time_point deadline,
                std::coroutine_handle<> handle,
                std::shared_ptr<std::atomic<unsigned char>> claim = {}) -> TimerTask* {
    /// @brief 创建由时间轮拥有的定时节点，取消仲裁状态与等待器共享而不借用等待器对象。
    auto task = std::make_unique<TimerTask>(deadline, handle, std::move(claim));
    /// @brief 保存可用于身份查找的非拥有节点指针，任务所有权仍由 unique_ptr 持有。
    auto* raw = task.get();
    /// @brief 把节点唯一所有权移入根时间轮，必要时按截止偏移升级层级。
    add_task_impl(std::move(task));
    /// @brief 返回已登记节点的借用身份，节点被到期或取消清理后该地址不可再使用。
    return raw;
  }

  /// @brief 移除定时器任务
  /// @param task 要移除的任务裸指针
  void remove_task(TimerTask* task) {
    /// @brief 空节点身份无需执行时间定位或链表移除。
    if (task == nullptr) {
      return;
    }

    /// @brief 读取单调时间，避免墙上时钟调整影响到期判定。
    auto now = std::chrono::steady_clock::now();
    /// @brief 如果任务已过期，无需移除
    if (task->_deadline <= now) {
      return;
    }

    /// @brief 所有索引均相对根轮起点；减去 elapsed 会把任务定位到其他槽位。
    auto interval_ms = to_ms(task->_deadline - _start);

    /// @brief 在根时间轮中递归移除
    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          /// @brief 编译期跳过表示尚未分配根轮的空备选项，只实例化真实层级的轮操作。
          if constexpr (!std::is_same_v<T, std::monostate>) {
            /// @brief 根轮指针有效时才递归访问对应层级的节点。
            if (wheel_ptr) {
              /// @brief 在当前根层级按统一绝对偏移递归摘除目标节点。
              wheel_ptr->remove_task(task, interval_ms);
              /// @brief 只在活跃计数非零时递减，避免无符号计数下溢。
              if (_num_entries > 0) {
                /// @brief 成功走完移除路径后归还一个已登记定时条目的计数。
                --_num_entries;
              }
              /// @brief 回收空根或把只有第零槽非空的高层轮降为子轮，减少长期空闲存储。
              try_level_down();
            }
          }
        },
        _root_wheel);
  }

  /// @brief 轮询处理到期任务
  /// @param sink 所属线程的就绪接收端
  /// @return 本次处理的到期任务数量
  template <ready_sink sink_type>
  auto poll(sink_type& sink) -> std::size_t {
    /// @brief 没有取消请求时只读取；避免每次空 IO 轮询都对取消计数执行原子 RMW。
    /// load 后新到的请求与原 exchange 后新到的请求一样，由后续 poll 领取。
    if (_cancel_requests.load(std::memory_order_acquire) != 0
        && _cancel_requests.exchange(0, std::memory_order_acq_rel) != 0) {
      /// @brief 累计本次取消清理实际摘除的任务数，调用方据此修正活跃计数。
      std::size_t removed = 0;
      /// @brief 按根变体实际层级分派轮操作；空备选项在编译期跳过，真实子轮仍保持唯一所有权。
      std::visit(
          [&](auto& wheel_ptr) {
            using T = std::decay_t<decltype(wheel_ptr)>;
            if constexpr (!std::is_same_v<T, std::monostate>)
              if (wheel_ptr)
                /// @brief 在所属线程统一回收取消节点；取消回调自身不修改轮树结构。
                removed = wheel_ptr->prune_cancelled();
          },
          _root_wheel);
      /// @brief 按真实回收数量减少登记计数，并限制减量避免无符号下溢。
      _num_entries -= std::min(_num_entries, removed);
      /// @brief 只有实际移除了条目时才尝试收缩层级。
      if (removed)
        /// @brief 回收空根或把只有第零槽非空的高层轮降为子轮，减少长期空闲存储。
        try_level_down();
    }
    /// @brief 没有剩余登记条目时直接结束本轮处理或重建新的时间起点。
    if (_num_entries == 0) {
      return 0;
    }

    /// @brief 计算相对当前根第零槽起点已经经过的完整毫秒数。
    auto elapsed = elapsed_ms();
    /// @brief 尚无完整毫秒可推进时保留现有坐标与节点，避免提前触发。
    if (elapsed == 0) {
      return 0;
    }

    /// @brief 累计此次轮询摘出的到期条目数。
    std::size_t count = 0;
    /// @brief 按根变体实际层级分派轮操作；空备选项在编译期跳过，真实子轮仍保持唯一所有权。
    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          /// @brief 编译期跳过表示尚未分配根轮的空备选项，只实例化真实层级的轮操作。
          if constexpr (!std::is_same_v<T, std::monostate>) {
            /// @brief 根轮指针有效时才递归访问对应层级的节点。
            if (wheel_ptr) {
              /// @brief 递归扫描相对根起点已经到期的前缀，并把恢复句柄交给所属就绪队列。
              wheel_ptr->handle_expired_tasks(sink, count, elapsed);
            }
          }
        },
        _root_wheel);

    /// @brief 只能推进已完整扫描的根槽位；高层轮的部分子轮仍沿用原来的起点。
    advance_start(elapsed);

    /// @brief 只有真正摘出了条目才修正计数并尝试收缩根轮。
    if (count > 0) {
      /// @brief 从活跃计数中扣除本轮摘出的条目，不按通知成功次数扣减。
      _num_entries -= std::min(_num_entries, count);
      /// @brief 尝试降级
      try_level_down();

      /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
      faio::log::logger()->trace(
          "Timer::poll: processed {} tasks, {} remaining", count, _num_entries);
    }

    /// @brief 返回此次从时间轮处理的到期条目数量，供事件循环判断是否取得进展。
    return count;
  }

  /// @brief 获取下一个到期任务的时间（距 now 的毫秒数）
  /// @return 下一个到期任务距离现在的毫秒数；若无任务返回 max
  [[nodiscard]]
  auto next_deadline_ms() const noexcept -> std::size_t {
    /// @brief 没有剩余登记条目时直接结束本轮处理或重建新的时间起点。
    if (_num_entries == 0) {
      /// @brief 用最大毫秒值表示没有待到期任务，宿主可选择无限内核等待。
      return std::numeric_limits<std::size_t>::max();
    }

    /// @brief 先取无截止时间哨兵，只有真实根轮包含任务时才写入有效等待时长。
    std::size_t expected = std::numeric_limits<std::size_t>::max();
    /// @brief 按根变体实际层级分派轮操作；空备选项在编译期跳过，真实子轮仍保持唯一所有权。
    std::visit(
        [&](const auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          /// @brief 编译期跳过表示尚未分配根轮的空备选项，只实例化真实层级的轮操作。
          if constexpr (!std::is_same_v<T, std::monostate>) {
            /// @brief 根轮指针有效时才递归访问对应层级的节点。
            if (wheel_ptr) {
              /// @brief 查询相对根轮起点的最早任务偏移，此时尚未扣除已经经过的时间。
              auto deadline_from_start = wheel_ptr->next_deadline_time();
              /// @brief 计算相对当前根第零槽起点已经经过的完整毫秒数。
              auto elapsed = elapsed_ms();
              /// @brief 最早任务仍在将来时，等待时长为绝对偏移减去已过毫秒。
              if (deadline_from_start > elapsed) {
                /// @brief 把根坐标偏移转换为距当前时刻的剩余等待毫秒数。
                expected = deadline_from_start - elapsed;
              } else {
                /// @brief 最早任务已经到期，宿主应非阻塞驱动而非继续等待。
                expected = 0;
              }
            }
          }
        },
        _root_wheel);

    /// @brief 向宿主返回距下一次定时完成的等待上限。
    return expected;
  }

  /// @brief 获取当前剩余定时器任务数
  [[nodiscard]]
  auto num_entries() const noexcept -> std::size_t {
    /// @brief 返回当前仍登记在轮中的条目数量，包含等待所属线程清理的取消项。
    return _num_entries;
  }

  /// @brief 判断定时器是否为空（没有待处理任务）
  [[nodiscard]]
  auto empty() const noexcept -> bool {
    /// @brief 以登记计数判断管理器是否为空。
    return _num_entries == 0;
  }

 private:
  /// @brief 内部添加任务的统一实现
  void add_task_impl(std::unique_ptr<TimerTask>&& task) {
    /// @brief 空轮没有旧任务约束，可以重新建立基准，避免长时间空闲扩大层级。
    if (_num_entries == 0) {
      /// @brief 空轮没有旧坐标约束，重新以当前单调时间建立根轮起点。
      _start = std::chrono::steady_clock::now();
      /// @brief 释放旧根所有权并进入未分配根轮状态。
      _root_wheel = std::monostate{};
    }

    /// @brief 活跃轮中即使 worker 很久没有 poll，也必须按旧根起点计算绝对槽位。
    /// 截止时间落在当前时刻之前时使用 0；下一次 poll 会处理该已过期任务。
    auto interval_ms = to_ms(task->_deadline - _start);

    /// @brief 确保根时间轮已初始化，且层级足够容纳此任务
    ensure_capacity(interval_ms);

    /// @brief 将任务添加到根时间轮
    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          /// @brief 编译期跳过表示尚未分配根轮的空备选项，只实例化真实层级的轮操作。
          if constexpr (!std::is_same_v<T, std::monostate>) {
            /// @brief 根轮指针有效时才递归访问对应层级的节点。
            if (wheel_ptr) {
              /// @brief 将唯一节点所有权递归移入当前足够容纳截止偏移的根轮。
              wheel_ptr->add_task(std::move(task), interval_ms);
            }
          }
        },
        _root_wheel);

    /// @brief 节点成功登记后才增加活跃计数，分配失败不留下虚假条目。
    ++_num_entries;
  }

  /// @brief 确保根时间轮的容量足够容纳 interval_ms
  /// 如果当前层级不够，自动升级（level_up）
  void ensure_capacity(std::size_t interval_ms) {
    /// @brief 如果根时间轮还没初始化，创建 level-0 时间轮
    if (std::holds_alternative<std::monostate>(_root_wheel)) {
      /// @brief 截止偏移在叶子轮跨度内时无需构造更高层级。
      if (interval_ms < TimerWheel<0uz>::SPAN_MS) {
        /// @brief 先建立最小叶子轮，后续容量检查再按需逐级包装升级。
        _root_wheel = std::make_unique<TimerWheel<0uz>>();
        return;
      }
      /// @brief 需要更高级别的时间轮
      _root_wheel = std::make_unique<TimerWheel<0uz>>();
    }

    /// @brief 递归升级直到容量足够
    ensure_capacity_visit(interval_ms);
  }

  /// @brief 利用 variant visit 实现递归升级
  void ensure_capacity_visit(std::size_t interval_ms) {
    /// @brief 按根变体实际层级分派轮操作；空备选项在编译期跳过，真实子轮仍保持唯一所有权。
    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          /// @brief 编译期跳过表示尚未分配根轮的空备选项，只实例化真实层级的轮操作。
          if constexpr (!std::is_same_v<T, std::monostate>) {
            /// @brief 根轮指针有效时才递归访问对应层级的节点。
            if (wheel_ptr) {
              using WheelType = std::remove_reference_t<decltype(*wheel_ptr)>;
              /// @brief 如果当前轮的跨度不足以容纳 interval_ms，需要升级
              if (interval_ms >= WheelType::SPAN_MS) {
                /// @brief 将当前根轮整体包装为更高层级，并继续验证新根能否覆盖截止偏移。
                level_up_variant(interval_ms);
              }
            }
          }
        },
        _root_wheel);
  }

  /// @brief 将根时间轮升级一级
  void level_up_variant(std::size_t interval_ms) {
    /// @brief 使用 index 实现编译期分发升级逻辑
    /// variant 的 index 0 是 monostate，1 对应 TimerWheel<0>,
    /// 2 对应 TimerWheel<1>, ...
    auto idx = _root_wheel.index();

    /// @brief 利用编译期索引序列实现升级分发
    level_up_dispatch(interval_ms, idx);
  }

  /// @brief 编译期分发升级逻辑的辅助函数
  void level_up_dispatch(std::size_t interval_ms, std::size_t current_idx) {
    /// @brief 逐级升级，MAX_LEVEL 为上限
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (
          [&] {
            /// @brief 把索引序列中的编译期下标解释为候选时间轮层级。
            constexpr std::size_t LEVEL = Is;
            /// @brief +1 因为 monostate
            constexpr std::size_t VARIANT_IDX = LEVEL + 1;
            /// @brief 只让与当前根变体相匹配的编译期分支执行升级。
            if (current_idx == VARIANT_IDX) {
              /// @brief 仅为尚未到达最大层级的轮实例化升级操作。
              if constexpr (LEVEL < MAX_LEVEL) {
                /// @brief 在已核对的变体下标处取得当前根所有者。
                auto& current_wheel = std::get<VARIANT_IDX>(_root_wheel);
                /// @brief 将旧根移入新父轮第零槽，旧根变量不再拥有原子树。
                auto new_wheel = current_wheel->level_up(std::move(current_wheel));
                /// @brief 以新父轮替换根变体，现存条目的时间坐标起点保持不变。
                _root_wheel = std::move(new_wheel);

                /// @brief 继续检查是否需要再升级
                ensure_capacity_visit(interval_ms);
              } else {
                faio::log::logger()->error("Timer: cannot level_up beyond MAX_LEVEL={}", MAX_LEVEL);
              }
            }
          }(),
          ...);
    }(std::make_index_sequence<MAX_LEVEL + 1>{});
  }

  /// @brief 尝试降级根时间轮
  /// 当只有第 0 个槽位有子轮时，可以降级以节省内存
  /// TimerWheel<0> 是最底层，不可再降级
  void try_level_down() {
    /// @brief 读取当前根变体下标，后续只进入对应真实层级的处理分支。
    auto idx = _root_wheel.index();
    /// @brief monostate (idx==0) 或 TimerWheel<0> (idx==1) 不需要降级
    if (idx <= 1) {
      /// @brief 对 TimerWheel<0>，仅检查是否为空可以释放
      if (idx == 1) {
        /// @brief 下标一对应叶子轮，叶子不能再降级，但空叶子可以直接回收。
        auto& wheel_ptr = std::get<1>(_root_wheel);
        /// @brief 根轮确实存在且已经无节点时可以释放整个根存储。
        if (wheel_ptr && wheel_ptr->empty()) {
          /// @brief 销毁已经排空的当前根轮。
          wheel_ptr.reset();
          /// @brief 释放旧根所有权并进入未分配根轮状态。
          _root_wheel = std::monostate{};
        }
      }
      return;
    }

    /// @brief 对 LEVEL >= 1 的时间轮，尝试降级
    level_down_dispatch();
  }

  /// @brief 编译期分发降级逻辑
  void level_down_dispatch() {
    /// @brief 读取当前根变体下标，后续只进入对应真实层级的处理分支。
    auto idx = _root_wheel.index();
    /// @brief 展开合法层级的编译期候选，只让匹配当前根变体下标的分支执行降级。
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (
          [&] {
            /// @brief 把索引序列中的编译期下标解释为候选时间轮层级。
            constexpr std::size_t LEVEL = Is;
            /// @brief 变体第零项保留给空状态，真实轮层级对应的变体下标需要加一。
            constexpr std::size_t VARIANT_IDX = LEVEL + 1;
            /// @brief 只对拥有子轮的合法高层级实例化降级逻辑。
            if constexpr (LEVEL >= 1 && LEVEL <= MAX_LEVEL) {
              /// @brief 变体下标匹配时才读取该层级指针，避免取得错误备选类型。
              if (idx == VARIANT_IDX) {
                /// @brief 取得已匹配的根轮所有者，后续移动须保持其唯一所有权。
                auto& wheel_ptr = std::get<VARIANT_IDX>(_root_wheel);
                /// @brief 已匹配备选项没有根轮对象时无需继续降级。
                if (!wheel_ptr) {
                  return;
                }
                /// @brief 整棵根轮没有任务时可退回完全未分配状态。
                if (wheel_ptr->empty()) {
                  /// @brief 销毁已经排空的当前根轮。
                  wheel_ptr.reset();
                  /// @brief 释放旧根所有权并进入未分配根轮状态。
                  _root_wheel = std::monostate{};
                } else if (wheel_ptr->can_level_down()) {
                  /// @brief 从高层根第零槽取出子轮，原根不再拥有该子树。
                  auto child = wheel_ptr->level_down();
                  /// @brief 确实取得子轮后才能用它替换根变体。
                  if (child) {
                    /// @brief 让被取出的子轮成为新根，原父轮在变体替换时销毁。
                    _root_wheel = std::move(child);
                    /// @brief 递归检查是否可以继续降级
                    try_level_down();
                  }
                }
              }
            }
          }(),
          ...);
    }(std::make_index_sequence<MAX_LEVEL + 1>{});
  }

  /// @brief 推进时间基准
  /// @param ms 从当前根轮起点起已经经过的毫秒数。
  /// @details 高层轮只能移动完整子轮的跨度。部分子轮已经扫描的前缀保持为空，
  /// 但它内部剩余任务的索引仍以原子轮起点为基准，不能减掉部分跨度。
  void advance_start(std::size_t ms) {
    /// @brief 按根变体实际层级分派轮操作；空备选项在编译期跳过，真实子轮仍保持唯一所有权。
    std::visit(
        [&](auto& wheel_ptr) {
          using T = std::decay_t<decltype(wheel_ptr)>;
          /// @brief 编译期跳过表示尚未分配根轮的空备选项，只实例化真实层级的轮操作。
          if constexpr (!std::is_same_v<T, std::monostate>) {
            /// @brief 根轮指针有效时才递归访问对应层级的节点。
            if (wheel_ptr) {
              using WheelType = std::remove_reference_t<decltype(*wheel_ptr)>;
              /// @brief 叶子层每槽 1ms，所有已扫描毫秒都可以从根坐标中移除。
              if constexpr (std::is_same_v<WheelType, TimerWheel<0uz>>) {
                /// @brief 叶子轮每槽一毫秒，可以直接移除已经完整扫描的毫秒前缀。
                wheel_ptr->rotate(ms);
                /// @brief 根槽移动多少毫秒，就同步把根时间起点推进相同跨度。
                _start += std::chrono::milliseconds(ms);
              } else {
                /// @brief 向下取整到完整子轮，保留 partial child 的内部坐标。
                auto slots_to_rotate = ms >> WheelType::CHILD_SHIFT;
                /// @brief 高层轮只有已经过去至少一个完整子轮时才移动根坐标。
                if (slots_to_rotate > 0) {
                  /// @brief 按完整子轮个数前移高层槽位，保留最后部分子轮的内部起点。
                  wheel_ptr->rotate(slots_to_rotate);
                  /// @brief 把移动的完整子轮数量换回毫秒，同步更新根第零槽的绝对时间。
                  _start += std::chrono::milliseconds(slots_to_rotate << WheelType::CHILD_SHIFT);
                }
              }
            }
          }
        },
        _root_wheel);
  }

  /// @brief 计算自启动以来经过的毫秒数
  [[nodiscard]]
  auto elapsed_ms() const noexcept -> std::size_t {
    /// @brief 读取单调时间，避免墙上时钟调整影响到期判定。
    auto now = std::chrono::steady_clock::now();
    /// @brief 将相对根起点的单调时间差向完整毫秒转换。
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - _start);
    /// @brief 返回已经经过的毫秒数量，供到期扫描和内核等待预算共用。
    return static_cast<std::size_t>(duration.count());
  }

  /// @brief 将 duration 转换为毫秒数
  static auto to_ms(std::chrono::steady_clock::duration dur) noexcept -> std::size_t {
    /// @brief 将任意单调时间跨度截取为完整毫秒。
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur);
    /// @brief 把已经过去的截止偏移钳为零，避免负时间转无符号后误成极远期任务。
    return static_cast<std::size_t>(std::max(ms.count(), decltype(ms)::rep{0}));
  }

 private:
  std::chrono::steady_clock::time_point _start{std::chrono::steady_clock::now()};  ///< 当前根轮第 0 槽的基准时间，始终与该层级的旋转跨度一致。
  std::size_t _num_entries{0};  ///< 当前活跃的定时器任务数
  std::atomic<std::size_t> _cancel_requests{0};  ///< 跨线程取消请求计数，所属线程据此按需清理时间轮。
  VariantWheelBuilder<MAX_LEVEL + 1uz>::Type _root_wheel{};  ///< 根时间轮（variant 存储，支持不同层级）
};
}
#endif
