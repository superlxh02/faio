/**
 * @file faio/detail/io/core/submitter_ref.hpp
 * @brief 提交器借用引用。
 */
#pragma once
#include "faio/detail/io/core/domain.hpp"
#include <memory>

namespace faio::io {
/**  @brief 请求注册者的提交协议；后台回调须另行保存 io_context 或 domain 租约。 */
template <class Derived>
class io_submitter_api {
 public:
  /** @brief 接受已准备的操作并负责其最终完成；按协议拒绝旧代际或重复提交。 */
  void submit(this const Derived& self, operation_token token) noexcept {
    if (auto* domain = self.native_handle())
      domain->submit(token);
  }

  /** @brief 按完整操作身份记录取消；原请求安全完成前不归还负载与槽位。 */
  void request_cancel(this const Derived& self,
                      operation_token token,
                      cancel_reason reason = cancel_reason::user) noexcept {
    if (auto* domain = self.native_handle())
      domain->request_cancel(token, reason);
  }
};

/** @brief 通过单个指针借用具体 Domain，不延长域的有效期，也不保存操作表。 */
template <class Domain>
class basic_io_submitter_ref : public io_submitter_api<basic_io_submitter_ref<Domain>> {
 public:
  using domain_type = Domain;
  /** @brief 构造静态 I/O 提交引用。 */
  basic_io_submitter_ref() = default;

  /** @brief 构造静态 I/O 提交引用。 */
  explicit basic_io_submitter_ref(Domain& domain) noexcept : domain_(std::addressof(domain)) {}

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return domain_ != nullptr; }

  /** @brief 借用底层 I/O 域。 */
  Domain* native_handle() const noexcept { return domain_; }

 private:
  Domain* domain_{};
};

using io_submitter_ref = basic_io_submitter_ref<detail::io_domain>;
}  // namespace faio::io
