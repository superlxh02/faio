/**
 * @file faio/detail/io/core/driver_ref.hpp
 * @brief 跨平台异步 IO：驱动器借用引用。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/io/core/domain.hpp"

namespace faio::io {
/** @brief 驱动器借用引用；不暴露 epoll/kqueue 或任何网络操作细节。 */
class io_driver_ref {
 public:
  io_driver_ref() = default;

  explicit io_driver_ref(detail::io_domain& domain) noexcept : domain_(&domain) {}

  /// @brief 在给定预算内推进后端、截止时间与完成发布。
  drive_result drive(drive_budget budget = {}) const noexcept {
    return domain_ ? domain_->drive(budget) : drive_result{0, false, false, ECANCELED};
  }

  /// @brief 等待可唤醒的底层事件后推进本域 IO 与定时任务。
  drive_result wait_and_drive(std::optional<int> timeout = {},
                              drive_budget budget = {}) const noexcept {
    return domain_ ? domain_->drive(budget, timeout) : drive_result{0, false, false, ECANCELED};
  }

  /// @brief 发送独立控制通知，打断底层等待以处理已经发布的工作。
  void wake() const noexcept {
    if (domain_)
      domain_->wake();
  }

  /// @brief 检查当前组件是否已经没有尚未归还的执行、内核引用与完成责任。
  bool quiescent() const noexcept { return !domain_ || domain_->quiescent(); }

 private:
  detail::io_domain* domain_{};  ///< 宿主 owning engine 覆盖借用 session 生命周期。
};
}
