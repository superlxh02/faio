#ifndef FAIO_DETAIL_RUNTIME_COMMON_WORKER_IO_BUDGET_HPP
#define FAIO_DETAIL_RUNTIME_COMMON_WORKER_IO_BUDGET_HPP

#include <chrono>
#include <utility>

namespace faio::runtime::detail {
/**
 * @brief worker 恢复边界的 IO 时间预算；空闲驱动保留最近真实计时驱动的旧锚点。
 * @details 时间锚点只能建立在真正 drive 开始以前，不能把稍后的 execute 时间
 *          冒充先前 poll 时刻。旧锚点不晚于后续空闲驱动，只会更早触发预算。
 *          回调均为静态模板调用，不拥有后端或计时器，
 *          不增加函数指针、分配、业务计数或线程。字段仅由所属 worker 访问。
 */
class worker_io_budget {
public:
  using clock_type = std::chrono::steady_clock;
  using time_point = clock_type::time_point;
  using duration = clock_type::duration;

  /// @brief 初始锚点由构造 worker 的调用方提供，不在空轮询中读取时钟。
  explicit worker_io_budget(time_point initial) noexcept
      : last_drive_(initial) {}

  /** @brief 空闲域驱动仍真正执行；不读时钟也绝不前移最近真实计时锚点。 */
  template <class Drive> decltype(auto) drive_without_anchor(Drive &&drive) {
    // 旧时间点早于本次调用，不会低估 elapsed；下一恢复边界仍检查原时间预算。
    return std::forward<Drive>(
        drive)(); // 直接执行原驱动，不缓存/延迟真实 IO 工作。
  }

  /** @brief 周期驱动先读取时间建立锚点，再立即执行原驱动；保留原 tick 语义。 */
  template <class Clock, class Drive>
  decltype(auto) drive_with_anchor(Clock &&clock, Drive &&drive) {
    last_drive_ = std::forward<Clock>(clock)(); // 必须在原 drive 开始以前采样。
    return std::forward<Drive>(
        drive)(); // 结果原样交回 worker 的批次/睡眠协议。
  }

  /**
   * @brief 每次选中任务时与保留的旧锚点比较，达到预算才真正驱动。
   * @return true 表示已经驱动，worker 必须随后重查 shutdown 再执行选中任务。
   * @details local 和 steal 使用同一入口。now 与真实 drive 在同一调用中衔接，
   *          长 drive 的耗时仍算入下一恢复边界预算，不会被结束时刻隐去。
   */
  template <class Clock, class Drive>
  bool refresh_before_execute(duration maximum_delay, Clock &&clock,
                              Drive &&drive) {
    const auto now =
        std::forward<Clock>(clock)(); // 持续就绪仍逐恢复检查 elapsed。
    if (now - last_drive_ < maximum_delay)
      return false;    // 旧锚点预算尚未耗尽，idle→active 不增加强制轮询。
    last_drive_ = now; // 先记 drive 开始边界，禁止事后以 execute 时刻前移它。
    std::forward<Drive>(
        drive)(); // 原 worker::drive_io_impl，仍中断 fast-chain 并发布完成。
    return true;  // 调用方现在重查关闭；不在此恢复或拥有 coroutine_handle。
  }

private:
  time_point last_drive_; // 最近一次带锚点驱动开始的保守时间。
};
} // namespace faio::runtime::detail

#endif
