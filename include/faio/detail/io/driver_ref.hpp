#pragma once
#include "faio/detail/io/core/domain.hpp"

namespace faio::io {
/** @brief 驱动器借用引用；不暴露 epoll/kqueue 或任何网络操作细节。 */
class io_driver_ref {
 public:
  io_driver_ref() = default;

  explicit io_driver_ref(detail::io_domain& domain) noexcept : domain_(&domain) {}

  drive_result drive(drive_budget budget = {}) const noexcept {
    return domain_ ? domain_->drive(budget) : drive_result{0, false, false, ECANCELED};
  }

  drive_result wait_and_drive(std::optional<int> timeout = {},
                              drive_budget budget = {}) const noexcept {
    return domain_ ? domain_->drive(budget, timeout) : drive_result{0, false, false, ECANCELED};
  }

  void wake() const noexcept {
    if (domain_)
      domain_->wake();
  }

  bool quiescent() const noexcept { return !domain_ || domain_->quiescent(); }

 private:
  detail::io_domain* domain_{};  ///< 宿主 owning engine 覆盖借用 session 生命周期。
};
}  // namespace faio::io
