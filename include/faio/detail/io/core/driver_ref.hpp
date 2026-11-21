/**
 * @file faio/detail/io/core/driver_ref.hpp
 * @brief 驱动器借用引用。
 */
#pragma once
#include "faio/detail/io/core/domain.hpp"
#include <memory>

namespace faio::io {
/**  @brief 驱动器借用引用；不暴露 epoll/kqueue 或任何网络操作细节。 */
template <class Derived>
class io_driver_api {
 public:
  /** @brief 在给定预算内推进后端、截止时间与完成发布。 */
  drive_result drive(this const Derived& self, drive_budget budget = {}) noexcept {
    auto* domain = self.native_handle();
    return domain ? domain->drive(budget) : drive_result{0, false, false, ECANCELED};
  }

  /** @brief 等待可唤醒的底层事件后推进本域 IO 与定时任务。 */
  drive_result wait_and_drive(this const Derived& self,
                              std::optional<int> timeout = {},
                              drive_budget budget = {}) noexcept {
    auto* domain = self.native_handle();
    return domain ? domain->drive(budget, timeout) : drive_result{0, false, false, ECANCELED};
  }

  /** @brief 通过独立控制通知唤醒底层等待，处理已发布的工作。 */
  void wake(this const Derived& self) noexcept {
    if (auto* domain = self.native_handle())
      domain->wake();
  }

  /** @brief 检查执行、内核引用和完成责任是否均已归还。 */
  bool quiescent(this const Derived& self) noexcept {
    auto* domain = self.native_handle();
    return !domain || domain->quiescent();
  }
};

/** @brief 借用具体 Domain；通过模板和显式对象参数静态选择所有驱动操作。 */
template <class Domain>
class basic_io_driver_ref : public io_driver_api<basic_io_driver_ref<Domain>> {
 public:
  using domain_type = Domain;
  /** @brief 构造静态 I/O 驱动引用。 */
  basic_io_driver_ref() = default;

  /** @brief 构造静态 I/O 驱动引用。 */
  explicit basic_io_driver_ref(Domain& domain) noexcept : domain_(std::addressof(domain)) {}

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return domain_ != nullptr; }

  /** @brief 借用底层 I/O 域。 */
  Domain* native_handle() const noexcept { return domain_; }

 private:
  Domain* domain_{}; /**< 宿主引擎须在借用会话期间保持有效。 */
};

using io_driver_ref = basic_io_driver_ref<detail::io_domain>;
}  // namespace faio::io
