/**
 * @file faio/detail/experimental/execution/default_runtime.hpp
 * @brief 实验性的入口注解与发送器接收器适配：默认运行时访问。
 * @details 实验适配在编译期开关下启用，连接阶段建立稳定操作状态；启动、结果交付与关闭沿受控任务图的租约和排空协议推进。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_DEFAULT_RUNTIME_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_DEFAULT_RUNTIME_HPP
#include "faio/detail/experimental/execution/run.hpp"
#include "faio/detail/runtime/default.hpp"
#include <functional>

namespace faio::experimental {
/// @brief 服务共享访问覆盖整个回调；借用的调度器和操作须在回调内结束，关闭则在访问结束后执行。
template <class Function>
decltype(auto) with_default_runtime(Function&& function) {
  if (faio::detail::on_runtime_worker())
    throw std::logic_error("faio experimental: use this_runtime on a runtime worker");
  return faio::runtime::detail::with_default_runtime([&](auto& context) -> decltype(auto) {
    return std::invoke(std::forward<Function>(function), runtime_ref{context});
  });
}
}
#endif
