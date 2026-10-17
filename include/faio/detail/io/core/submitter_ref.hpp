/**
 * @file faio/detail/io/core/submitter_ref.hpp
 * @brief 跨平台异步 IO：提交器借用引用。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/io/core/domain.hpp"

namespace faio::io {
/** @brief 注册者提交协议；后台 callback 须另外保存 io_context/domain lease。 */
class io_submitter_ref {
 public:
  io_submitter_ref() = default;

  explicit io_submitter_ref(detail::io_domain& domain) noexcept : domain_(&domain) {}

  /// @brief 接受已准备的操作并承担最终完成责任；旧代际或重复提交按协议拒绝。
  void submit(operation_token token) const noexcept {
    if (domain_)
      domain_->submit(token);
  }

  /// @brief 按完整操作身份记录取消；原请求安全完成前不归还负载与槽位。
  void request_cancel(operation_token token,
                      cancel_reason reason = cancel_reason::user) const noexcept {
    if (domain_)
      domain_->request_cancel(token, reason);
  }

 private:
  detail::io_domain* domain_{};  ///< 借用已绑定的 IO 域，不延长域生命期。
};
}
