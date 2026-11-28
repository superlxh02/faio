/**
 * @file faio/detail/coroutine/scheduler/policy.hpp
 * @brief 可继承的任务调度约束，与调度器实例及协作预算分开保存。
 */
#pragma once

#include <cstddef>
#include <limits>
#include <cstdint>

namespace faio::detail {
inline constexpr std::size_t no_bound_worker = std::numeric_limits<std::size_t>::max();

enum class scheduling_setting : std::uint8_t { INHERIT, ENABLED, DISABLED };

/** @brief 只描述本任务显式请求；任务启动时与父级有效约束合并。 */
struct task_scheduling_options {
  scheduling_setting stealing{scheduling_setting::INHERIT};
  scheduling_setting bind_core{scheduling_setting::INHERIT};
};

/** @brief 生效后的策略。禁止窃取不固定 worker；绑核则固定 worker 和它的 CPU。 */
struct task_scheduling_policy {
  bool no_stealing{};
  std::size_t bound_worker{no_bound_worker};
  int bound_cpu{-1};

  /** @brief 判断任务是否绑定工作线程。 */
  bool binds_core() const noexcept { return bound_worker != no_bound_worker; }

  /** @brief 比较两个对象的值。 */
  friend bool operator==(const task_scheduling_policy&, const task_scheduling_policy&) = default;
};

}  // namespace faio::detail
