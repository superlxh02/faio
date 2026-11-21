/**
 * @file faio/detail/coroutine/execution_adapter/completion_reference.hpp
 * @brief 完成发布服务的模板借用接口；不包含具体宿主或 IO 实现。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/task_completion_service.hpp"
#include <concepts>
#include <memory>

namespace faio::detail {
template <class Service>
concept task_completion = requires(Service& service) {
  { service.begin_publisher() } noexcept -> std::same_as<void>;
  { service.end_publisher() } noexcept -> std::same_as<void>;
  { service.mark_tracker_zero() } noexcept -> std::same_as<void>;
};

/** @brief 任务终止与排空通知接口。 */
template <class Derived>
class task_completion_api {
 protected:
  /** @brief 构造任务终止与排空通知接口。 */
  task_completion_api() = default;
  /** @brief 销毁任务终止与排空通知接口。 */
  ~task_completion_api() = default;

 public:
  /** @brief 登记进行中的结果发布者。 */
  void begin_publisher(this const Derived& self) noexcept {
    if (auto* service = self.native_handle())
      service->begin_publisher();
  }

  /** @brief 归还结果发布者计数。 */
  void end_publisher(this const Derived& self) noexcept {
    if (auto* service = self.native_handle())
      service->end_publisher();
  }

  /** @brief 发布任务计数归零状态。 */
  void mark_tracker_zero(this const Derived& self) noexcept {
    if (auto* service = self.native_handle())
      service->mark_tracker_zero();
  }
};

/** @brief 任务完成服务的静态借用引用。 */
template <task_completion Service>
class basic_task_completion_ref : public task_completion_api<basic_task_completion_ref<Service>> {
 public:
  /** @brief 构造任务完成服务的静态借用引用。 */
  basic_task_completion_ref() noexcept = default;

  /** @brief 构造任务完成服务的静态借用引用。 */
  explicit basic_task_completion_ref(Service& service) noexcept
      : service_(std::addressof(service)) {}

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return service_ != nullptr; }

  /** @brief 借用底层原生句柄。 */
  Service* native_handle() const noexcept { return service_; }

 private:
  Service* service_{};
};

using task_completion_ref = basic_task_completion_ref<task_completion_service>;
}  // namespace faio::detail

#endif
