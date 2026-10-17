/**
 * @file faio/detail/fs/core/active_operations.hpp
 * @brief 文件与目录异步接口：活跃文件操作登记。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once

#include "faio/detail/coroutine/coroutine_wait.hpp"
#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace faio::fs::detail {
/** @brief 文件租约登记；调用方持有 file_state::mutex 后访问。
 * @details 操作块由租约、登记表和 flush/close 快照共同拥有，不拥有 File。
 *          索引随末项换位更新，因此 admission 和释放都无需扫描旧的弱引用。
 */
class active_file_operations {
 public:
  struct operation {
    explicit operation(bool writing) noexcept : write(writing) {}

    ::faio::detail::completion_event completion;  ///< 带记忆的完成事件，通知后晚到等待者也可直接继续。
    bool write;  ///< 本操作是否写入，用于关闭与同步时区分活跃读写。

   private:
    friend class active_file_operations;
    std::size_t index{};  ///< 当前操作在活跃操作表中的索引，便于常数时间移除。
  };

  using handle = std::shared_ptr<operation>;  ///< 共享持有活跃操作记录的租约类型。
  using snapshot_type = std::vector<handle>;  ///< 共享持有活跃操作的快照类型，等待期间固定各操作的生命期。

  active_file_operations() = default;
  active_file_operations(const active_file_operations&) = delete;
  active_file_operations& operator=(const active_file_operations&) = delete;
  active_file_operations(active_file_operations&&) = delete;
  active_file_operations& operator=(active_file_operations&&) = delete;

  void insert(const handle& entry) {
    entry->index = active_.size();
    active_.push_back(entry);
  }

  void remove(const handle& entry) noexcept {
    const auto index = entry->index;
    assert(index < active_.size() && active_[index] == entry);
    if (index + 1 != active_.size()) {
      active_[index] = std::move(active_.back());
      active_[index]->index = index;
    }
    active_.pop_back();
  }

  /** @brief 快照拥有事件；随后移除登记或接受新写入都不改变屏障集合。 */
  snapshot_type capture(bool writes_only = false) const {
    snapshot_type result;
    for (const auto& entry : active_)
      if (!writes_only || entry->write)
        result.push_back(entry);
    return result;
  }

  /// @brief 检查当前对象是否没有可消费的数据或任务。
  bool empty() const noexcept { return active_.empty(); }

  /// @brief 读取当前对象的有效元素或字节数量，线程访问约束由所属类型规定。
  std::size_t size() const noexcept { return active_.size(); }

 private:
  std::vector<handle> active_;  ///< 记录尚未安全完成的文件操作，关闭与同步查询据此等待排空。
};
}
