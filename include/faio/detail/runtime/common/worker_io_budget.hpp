/**
 * @file faio/detail/runtime/common/worker_io_budget.hpp
 * @brief 工作线程 IO 延迟预算。
 */
#ifndef FAIO_DETAIL_RUNTIME_COMMON_WORKER_IO_BUDGET_HPP
#define FAIO_DETAIL_RUNTIME_COMMON_WORKER_IO_BUDGET_HPP

#include <chrono>
#include <utility>

namespace faio::runtime::detail {
/**
 * @brief 工作线程恢复协程时使用的 I/O 时间预算；空闲驱动保留最近一次实际计时驱动的时间基准。
 * @details 时间基准必须记录在实际 drive 开始前，不能用之后的 execute 时刻代替先前的 poll 时刻。
 *          旧基准不晚于后续空闲驱动，因此只会让预算更早耗尽。
 *          回调均为静态模板调用，不拥有后端或计时器，
 *          不增加函数指针、分配、业务计数或线程。字段仅由所属 worker 访问。
 */
class worker_io_budget {
 public:
  using clock_type =
      std::chrono::steady_clock; /**< 预算计时使用的单调时钟类型，不受系统时间校准影响。 */
  using time_point = clock_type::time_point; /**< 单调时钟的绝对时间点类型。 */
  using duration = clock_type::duration;     /**< 单调时钟的时间跨度类型。 */

  /** @brief 初始锚点由构造 worker 的调用方提供，不在空轮询中读取时钟。 */
  explicit worker_io_budget(time_point initial) noexcept : last_drive_(initial) {}

  /**  @brief 空闲时仍实际驱动域，但不读取时钟，也不前移最近一次计时驱动的时间基准。 */
  template <class Drive>
  decltype(auto) drive_without_anchor(Drive&& drive) {
    return std::forward<Drive>(drive)();
  }

  /**  @brief 周期驱动先读取时间建立锚点，再立即执行原驱动；保留原 tick 语义。 */
  template <class Clock, class Drive>
  decltype(auto) drive_with_anchor(Clock&& clock, Drive&& drive) {
    last_drive_ = std::forward<Clock>(clock)();
    return std::forward<Drive>(drive)();
  }

  /**
   * @brief 每次选中任务时与保留的旧锚点比较，达到预算才真正驱动。
   * @return true 表示已经驱动，worker 必须随后重查 shutdown 再执行选中任务。
   * @details local 和 steal 使用同一入口。now 与真实 drive 在同一调用中衔接，
   *          耗时较长的 drive 仍计入下一次恢复时的预算，不会因改用结束时刻而被忽略。
   */
  template <class Clock, class Drive>
  bool refresh_before_execute(duration maximum_delay, Clock&& clock, Drive&& drive) {
    const auto now = std::forward<Clock>(clock)();
    if (now - last_drive_ < maximum_delay)
      return false;
    last_drive_ = now;
    std::forward<Drive>(drive)();
    return true;
  }

 private:
  time_point last_drive_; /**< 最近一次带锚点驱动开始的保守时间。 */
};
}  // namespace faio::runtime::detail

#endif
