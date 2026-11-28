/**
 * @file faio/detail/experimental/execution/default_runtime.hpp
 * @brief 当前执行上下文的桥接入口与默认运行时访问。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_DEFAULT_RUNTIME_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_DEFAULT_RUNTIME_HPP
#include "faio/detail/experimental/execution/run.hpp"
#include "faio/detail/experimental/execution/spawn_sender.hpp"
#include "faio/detail/experimental/execution/task_sender.hpp"
#include "faio/detail/runtime/default.hpp"
#include <functional>
#include <utility>

namespace faio::experimental {
/**
 * @brief 把惰性 faio 任务转换为使用当前执行上下文的 sender。
 * @tparam T 任务成功时返回的类型，可以是 void。
 * @param child 移交给 sender 的任务。转换时不会启动任务。
 * @return 接入当前执行上下文的 sender，启动时接管任务的执行与终态交付。
 * @throws std::logic_error 调用线程没有绑定 faio 执行上下文。
 */
template <class T>
auto as_sender(faio::task<T> child) {
  return this_runtime().as_sender(std::move(child));
}

/**
 * @brief 向当前执行上下文提交 sender 图，并沿用当前任务的停止与排空责任。
 * @tparam sender_type 要提交的 sender 类型。
 * @param sender 立即提交的 sender。
 * @return 一次性等待句柄。成功时交付值元组，停止时交付空结果，失败时抛出异常。
 * @throws std::logic_error 调用线程没有绑定 faio 执行上下文。
 */
template <class sender_type>
auto spawn_sender(sender_type&& sender) {
  return this_runtime().spawn_sender(std::forward<sender_type>(sender));
}

/** @brief 服务共享访问覆盖整个回调；借用的调度器和操作须在回调内结束，关闭则在访问结束后执行。 */
template <class Function>
decltype(auto) with_default_runtime(Function&& function) {
  if (faio::detail::on_runtime_worker())
    throw std::logic_error("faio experimental: use this_runtime on a runtime worker");
  return faio::runtime::detail::with_default_runtime([&](auto& context) -> decltype(auto) {
    return std::invoke(std::forward<Function>(function), runtime_ref{context});
  });
}
}  // namespace faio::experimental
#endif
