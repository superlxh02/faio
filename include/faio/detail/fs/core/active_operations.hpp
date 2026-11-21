/**
 * @file faio/detail/fs/core/active_operations.hpp
 * @brief 活跃文件操作登记。
 */
#pragma once

#include "faio/detail/coroutine/wait/completion_event.hpp"
#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace faio::fs::detail {
/**
 *  @brief 文件租约登记表，调用方须持有 file_state::mutex 后再访问。
 * @details 租约、登记表和 flush/close 快照共同持有操作块，操作块不拥有 File。
 *          移除时用末项换位并更新索引，因此接纳和释放操作时都无需扫描旧的弱引用。
 */
class active_file_operations {
 public:
  /** @brief 活跃文件操作的共享完成记录及读写属性。 */
  struct operation {
    /** @brief 建立带读写属性的文件操作完成记录。 */
    explicit operation(bool writing) noexcept : write(writing) {}

    ::faio::detail::completion_event
        completion; /**< 保留完成状态的事件；通知后才到达的等待者也可直接继续执行。 */
    bool write;     /**< 本操作是否写入，用于关闭与同步时区分活跃读写。 */

   private:
    friend class active_file_operations;
    std::size_t index{}; /**< 当前操作在活跃操作表中的索引，便于常数时间移除。 */
  };

  using handle = std::shared_ptr<operation>; /**< 共享持有活跃操作记录的租约类型。 */
  using snapshot_type =
      std::vector<handle>; /**< 共享持有活跃操作的快照类型，在等待期间保持各操作存活。 */

  /** @brief 构造空的活跃文件操作登记表。 */
  active_file_operations() = default;
  /** @brief 禁止复制此对象。 */
  active_file_operations(const active_file_operations&) = delete;
  /** @brief 禁止复制赋值。 */
  active_file_operations& operator=(const active_file_operations&) = delete;
  /** @brief 禁止移动此对象。 */
  active_file_operations(active_file_operations&&) = delete;
  /** @brief 禁止移动赋值。 */
  active_file_operations& operator=(active_file_operations&&) = delete;

  /** @brief 登记活跃文件操作并记录其移除索引。 */
  void insert(const handle& entry) {
    entry->index = active_.size();
    active_.push_back(entry);
  }

  /** @brief 移除活跃文件操作并更新换位项的索引。 */
  void remove(const handle& entry) noexcept {
    const auto index = entry->index;
    assert(index < active_.size() && active_[index] == entry);
    if (index + 1 != active_.size()) {
      active_[index] = std::move(active_.back());
      active_[index]->index = index;
    }
    active_.pop_back();
  }

  /**  @brief 快照持有事件的所有权；后续移除登记或接纳新写入，都不会改变本次屏障等待的操作集合。 */
  snapshot_type capture(bool writes_only = false) const {
    snapshot_type result;
    for (const auto& entry : active_)
      if (!writes_only || entry->write)
        result.push_back(entry);
    return result;
  }

  /** @brief 判断活跃文件操作登记表是否为空。 */
  bool empty() const noexcept { return active_.empty(); }

  /** @brief 返回当前登记的活跃文件操作数量。 */
  std::size_t size() const noexcept { return active_.size(); }

 private:
  std::vector<handle> active_; /**< 记录尚未安全完成的文件操作，关闭和同步操作据此等待排空。 */
};
}  // namespace faio::fs::detail
