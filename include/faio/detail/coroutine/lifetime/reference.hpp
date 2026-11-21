/**
 * @file faio/detail/coroutine/lifetime/reference.hpp
 * @brief 根任务生命周期服务的模板借用接口。
 */
#pragma once

#include "faio/detail/runtime/common/task_lifetime_service.hpp"
#include <concepts>
#include <memory>

namespace faio {
template <class L>
concept task_lifetime = requires(L& lifetime) {
  { lifetime.register_task() } noexcept -> std::same_as<void>;
  { lifetime.finish_task() } noexcept -> std::same_as<void>;
};

/** @brief 根任务生命周期登记接口。 */
template <class Derived>
class task_lifetime_api {
 protected:
  /** @brief 构造根任务生命周期登记接口。 */
  task_lifetime_api() = default;
  /** @brief 销毁根任务生命周期登记接口。 */
  ~task_lifetime_api() = default;

 public:
  /** @brief 登记根任务的完成责任。 */
  void register_task(this const Derived& self) noexcept {
    if (auto* service = self.native_handle())
      service->register_task();
  }

  /** @brief 归还根任务计数。 */
  void finish_task(this const Derived& self) noexcept {
    if (auto* service = self.native_handle())
      service->finish_task();
  }
};

/** @brief 单指针非拥有借用；空服务不登记，不保存操作表或回调。 */
template <task_lifetime L>
class basic_task_lifetime_ref : public task_lifetime_api<basic_task_lifetime_ref<L>> {
 public:
  /** @brief 构造静态任务生命周期引用。 */
  basic_task_lifetime_ref() noexcept = default;

  /** @brief 构造静态任务生命周期引用。 */
  explicit basic_task_lifetime_ref(L& service) noexcept : service_(std::addressof(service)) {}

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return service_ != nullptr; }

  /** @brief 借用底层原生句柄。 */
  L* native_handle() const noexcept { return service_; }

 private:
  L* service_{};
};

using task_lifetime_ref = basic_task_lifetime_ref<detail::task_lifetime_service>;
}  // namespace faio
