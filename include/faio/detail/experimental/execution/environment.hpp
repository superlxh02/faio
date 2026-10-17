/**
 * @file faio/detail/experimental/execution/environment.hpp
 * @brief 实验性的入口注解与发送器接收器适配：发送器环境与停止传播。
 * @details 实验适配在编译期开关下启用，连接阶段建立稳定操作状态；启动、结果交付与关闭沿受控任务图的租约和排空协议推进。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_ENVIRONMENT_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_ENVIRONMENT_HPP
#include "faio/detail/experimental/execution/scheduler.hpp"

namespace faio::experimental::detail {
template <faio::runtime::mode Mode, class Base = stdexec::env<>>
struct root_environment {
  execution_scheduler<Mode> scheduler;  ///< 捕获的目标调度器，跨线程完成时将续体投递回所属运行时。
  root_handle root;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
  Base base{};  ///< 底层环境或定位基准，按所属类型的协议解释。
  auto query(stdexec::get_start_scheduler_t) const noexcept -> execution_scheduler<Mode> { return scheduler; }
  auto query(stdexec::get_scheduler_t) const noexcept -> execution_scheduler<Mode> { return scheduler; }
  auto query(stdexec::get_stop_token_t) const noexcept -> root_stop_token { return root_stop_token{root->stop_token()}; }
  auto query(get_external_root_t) const noexcept -> root_handle { return root; }
  template <class Query>
    requires (!std::same_as<Query, stdexec::get_start_scheduler_t> &&
              !std::same_as<Query, stdexec::get_scheduler_t> &&
              !std::same_as<Query, stdexec::get_stop_token_t> &&
              !std::same_as<Query, get_external_root_t> &&
              requires(Query q, const Base& b) { q(b); })
  decltype(auto) query(Query q) const noexcept(noexcept(q(base))) { return q(base); }
};

struct request_root_stop {
  faio::detail::external_root_state* root{};  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
  void operator()() const noexcept { root->request_stop(); }
};
template <class Token, class Callback>
using stop_callback = stdexec::stop_callback_for_t<Token, Callback>;
}
#endif
