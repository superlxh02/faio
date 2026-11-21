/**
 * @file faio/detail/experimental/execution/backend.hpp
 * @brief 后端封装与提交协议。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_BACKEND_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_BACKEND_HPP
#include "faio/detail/experimental/platform.hpp"
#if !defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) || !FAIO_EXPERIMENTAL_HAS_EXECUTION
#error "faio experimental: execution capability is not enabled"
#endif
#include <stdexec/execution.hpp>
#include <stop_token>
#include <utility>

namespace faio::experimental::detail {
/** @brief 当前后端的 exec::task 要求令牌公开 callback_type；所有权与取消状态仍由标准停止状态保存。 */
class root_stop_token {
 public:
  template <class Callback>
  using callback_type = std::stop_callback<Callback>;
  /** @brief 构造不关联停止状态的令牌。 */
  root_stop_token() noexcept = default;

  /** @brief 共享标准停止令牌的状态。 */
  explicit root_stop_token(std::stop_token token) noexcept : token_(std::move(token)) {}

  /** @brief 检查根任务是否收到停止请求。 */
  bool stop_requested() const noexcept { return token_.stop_requested(); }

  /** @brief 检查停止令牌是否关联可停止状态。 */
  bool stop_possible() const noexcept { return token_.stop_possible(); }

  /** @brief 返回共享同一停止状态的标准令牌。 */
  operator std::stop_token() const noexcept { return token_; }

  /** @brief 比较两个令牌是否关联同一停止状态。 */
  bool operator==(const root_stop_token&) const noexcept = default;

 private:
  std::stop_token token_; /**< 保存停止令牌，在异步执行期间观察所属任务的取消请求。 */
};

static_assert(stdexec::stoppable_token<root_stop_token>);
}  // namespace faio::experimental::detail
#endif
