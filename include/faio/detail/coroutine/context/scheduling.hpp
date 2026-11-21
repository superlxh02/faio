/**
 * @file faio/detail/coroutine/context/scheduling.hpp
 * @brief 在任务启动边界合并显式调度请求与父任务的有效约束。
 */
#pragma once

#include "faio/detail/coroutine/context/execution_binding.hpp"
#include "faio/detail/coroutine/scheduler/policy.hpp"
#include "faio/detail/coroutine/scheduler/resume_target.hpp"
#include "faio/detail/log/logger.hpp"
#include <stdexcept>

namespace faio::detail {
/** @brief 记录继承窃取策略冲突的诊断。 */
inline void warn_inherited_stealing_conflict() noexcept {
  try {
    faio::log::logger()->warn(
        "task scheduling conflict: stealing requested=enabled inherited=disabled effective=disabled");
  } catch (...) {
  }
}

/** @brief 记录继承调度绑定冲突的诊断。 */
inline void warn_inherited_binding_conflict(const task_scheduling_policy& inherited) noexcept {
  try {
    faio::log::logger()->warn(
        "task scheduling conflict: bind_core requested=disabled inherited=enabled "
        "effective=enabled worker={} cpu={}",
        inherited.bound_worker,
        inherited.bound_cpu);
  } catch (...) {
  }
}

/** @brief 仅在首次启动时解析；后代可以收紧约束，请求放宽时发出警告并沿用父级约束。 */
template <class S>
task_scheduling_policy resolve_task_scheduling(task_scheduling_options requested,
                                               task_scheduling_policy inherited) {
  if (requested.bind_core == scheduling_setting::enabled
      && requested.stealing == scheduling_setting::enabled && !inherited.binds_core())
    throw std::logic_error("绑核任务不能同时显式允许窃取");

  auto effective = inherited;
  if (requested.stealing == scheduling_setting::enabled) {
    if (inherited.no_stealing || inherited.binds_core())
      warn_inherited_stealing_conflict();
    else
      effective.no_stealing = false;
  } else if (requested.stealing == scheduling_setting::disabled) {
    effective.no_stealing = true;
  }

  if (inherited.binds_core()) {
    if (requested.bind_core == scheduling_setting::disabled)
      warn_inherited_binding_conflict(inherited);
    effective.no_stealing = true;
  } else if (requested.bind_core == scheduling_setting::enabled) {
    effective.bound_worker = current_worker_id();
    effective.bound_cpu = current_bound_cpu();
    if (effective.bound_worker == no_worker_id || effective.bound_cpu < 0)
      throw std::logic_error("with_bind_core 需要已验证 CPU 亲和性的运行时 worker");
    effective.no_stealing = true;
  }

  if (effective.no_stealing || effective.binds_core()) {
    (void)basic_resume_target<S>{require_current_scheduler_for<S>(), effective};
  }
  return effective;
}
}  // namespace faio::detail
