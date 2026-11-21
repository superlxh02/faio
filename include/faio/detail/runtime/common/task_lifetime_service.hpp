/**
 * @file faio/detail/runtime/common/task_lifetime_service.hpp
 * @brief 内置运行时共用的具体根任务生命周期服务。
 */
#pragma once

#include "faio/detail/runtime/common/root_task_counter.hpp"

namespace faio::detail {
/** @brief 运行时根任务发布与排空服务。 */
class runtime_completion_service;

/** @brief 运行时拥有计数服务；所有借用只保存本类型的指针。 */
class task_lifetime_service {
 public:
  /** @brief 构造根任务生命周期服务。 */
  task_lifetime_service() noexcept = default;

  /** @brief 构造根任务生命周期服务。 */
  explicit task_lifetime_service(runtime_completion_service& completion) noexcept
      : completion_(&completion) {}

  /** @brief 登记根任务的完成责任。 */
  void register_task() noexcept { counter.register_task(); }

  /** @brief 归还根任务计数。 */
  void finish_task() noexcept;

  /** @brief 读取当前计数。 */
  std::size_t count() const noexcept { return counter.count(); }

  runtime::detail::root_task_counter counter;

 private:
  runtime_completion_service* completion_{};
};
}  // namespace faio::detail
