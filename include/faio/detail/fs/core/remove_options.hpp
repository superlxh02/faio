/**
 * @file faio/detail/fs/core/remove_options.hpp
 * @brief 递归删除预算。
 */
#pragma once
#include <cstddef>

namespace faio::fs {
/**
 *  @brief
 * 递归删除的资源上限。深度包含根目录，batch_size 限制单个文件任务的枚举步数。
 */
struct recursive_remove_options {
  std::size_t max_depth{256}; /**< 同时打开的目录数上限，遍历不递归消耗 C++ 调用栈。 */
  std::size_t max_state_bytes{1024 * 1024}; /**< 根路径及所持有名称的总内存上限。 */
  std::size_t batch_size{64};               /**< 每个任务的最大遍历步数，不能超过 4096。 */
};
}  // namespace faio::fs
