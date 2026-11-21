/**
 * @file faio/detail/runtime/execution_adapter/external_work_types.hpp
 * @brief 外部任务图的完整值型身份与前置声明，不装配调度器或宿主实现。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include <memory>

namespace faio::detail {
/** @brief 外部执行图的生命周期宿主。 */
class external_work_host;
/** @brief 外部执行图根状态。 */
struct external_root_state;
/** @brief 外部执行图的生命周期节点。 */
struct external_work_node;
/** @brief 外部执行图子任务计数租约。 */
class external_child_lease;
using external_root_handle = std::shared_ptr<external_root_state>;

/** @brief 只借用稳定宿主和根状态，有效性检查在完整宿主装配后定义。 */
struct external_scope_ref {
  external_work_host* host{};        /**< 所属宿主，既有接纳责任保证其存活。 */
  external_root_state* root_state{}; /**< 所属外部根状态，不转移其拥有关系。 */
  explicit operator bool() const noexcept;
  /** @brief 判断对象状态是否有效。 */
  bool valid() const noexcept;
};
}  // namespace faio::detail

#endif
