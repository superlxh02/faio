/**
 * @file faio/detail/experimental/execution/backend.hpp
 * @brief 实验性的入口注解与发送器接收器适配：后端封装与提交协议。
 * @details 实验适配在编译期开关下启用，连接阶段建立稳定操作状态；启动、结果交付与关闭沿受控任务图的租约和排空协议推进。
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
/// @brief 当前后端的 exec::task 要求令牌公开 callback_type；所有权与取消状态仍由标准停止状态保存。
class root_stop_token {
 public:
  template <class Callback> using callback_type = std::stop_callback<Callback>;
  root_stop_token() noexcept = default;
  explicit root_stop_token(std::stop_token token) noexcept : token_(std::move(token)) {}
  bool stop_requested() const noexcept { return token_.stop_requested(); }
  bool stop_possible() const noexcept { return token_.stop_possible(); }
  operator std::stop_token() const noexcept { return token_; }
  bool operator==(const root_stop_token&) const noexcept = default;
 private:
  std::stop_token token_;  ///< 捕获的停止令牌，在异步执行期间观察所属任务的取消请求。
};
static_assert(stdexec::stoppable_token<root_stop_token>);
}
#endif
