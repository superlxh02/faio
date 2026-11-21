/**
 * @file faio/detail/coroutine/execution_adapter/context.hpp
 * @brief execution 任务图身份的线程存储与上下文传播。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/context/current_context.hpp"
#include <concepts>
#include <coroutine>

namespace faio::detail {
/** @brief 结构化任务组在创建时捕获外部图身份。 */
struct execution_scope_context {
  external_scope_ref external_scope{current_task_context().external_scope};
};

/** @brief 取得继承的执行图作用域。 */
inline external_scope_ref inherited_execution_scope(bool inherit) noexcept {
  return inherit ? current_task_context().external_scope : external_scope_ref{};
}

template <class Promise>
concept execution_awaiter_context = requires(Promise& promise) {
  { promise.context.external_scope } -> std::same_as<external_scope_ref&>;
};
}  // namespace faio::detail
#endif
