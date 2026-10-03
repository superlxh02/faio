#pragma once
#include "faio/detail/io/core/domain.hpp"

namespace faio::io {
/** @brief 注册者提交协议；后台 callback 须另外保存 io_context/domain lease。 */
class io_submitter_ref {
 public:
  io_submitter_ref() = default;

  explicit io_submitter_ref(detail::io_domain& domain) noexcept : domain_(&domain) {}

  void submit(operation_token token) const noexcept {
    if (domain_)
      domain_->submit(token);
  }

  void request_cancel(operation_token token,
                      cancel_reason reason = cancel_reason::user) const noexcept {
    if (domain_)
      domain_->request_cancel(token, reason);
  }

 private:
  detail::io_domain* domain_{};
};
}  // namespace faio::io
