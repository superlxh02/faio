/**
 * @file faio/detail/runtime/execution_adapter/task_completion_service.hpp
 * @brief 任务组终态发布与独立发布者租约的具体共享服务。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION

#include "faio/detail/runtime/execution_adapter/external_work_types.hpp"
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace faio::detail {
/**
 * @brief 桥接状态拥有本服务；共享控制块仍按原始最派生类型销毁。
 * 计数器只借用本类型，接收器的具体交付由拥有的终态操作节点负责。
 */
class task_completion_service : public std::enable_shared_from_this<task_completion_service> {
 public:
  using root_handle = external_root_handle;

  /** @brief 构造任务完成发布服务。 */
  explicit task_completion_service(root_handle graph = {}) : root(std::move(graph)) {}

  /** @brief 绑定任务终止节点。 */
  void bind_terminal_node(external_work_node& node) noexcept { terminal_node_ = &node; }

  /** @brief 登记进行中的结果发布者。 */
  void begin_publisher() noexcept { publishers.fetch_add(1, std::memory_order_acq_rel); }

  /** @brief 归还结果发布者计数。 */
  void end_publisher() noexcept {
    auto lease = shared_from_this();
    if (publishers.fetch_sub(1, std::memory_order_acq_rel) == 1)
      try_terminal();
  }

  /** @brief 发布任务计数归零状态。 */
  void mark_tracker_zero() noexcept {
    tracker_zero.store(true, std::memory_order_release);
    try_terminal();
  }

  /** @brief 完整宿主装配后定义终态投递，声明层不引入宿主队列。 */
  void try_terminal() noexcept;

  root_handle root;
  std::atomic<bool> body_done{false};
  std::atomic<bool> tracker_zero{false};
  std::atomic<bool> terminal_queued{false};
  std::atomic<std::size_t> publishers{0};
  std::shared_ptr<task_completion_service> terminal_keepalive;

 private:
  external_work_node* terminal_node_{};
};

}  // namespace faio::detail

#endif
