/**
 * @file faio/detail/fs/core/remove_options.hpp
 * @brief 文件与目录异步接口：递归删除预算。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include <cstddef>

namespace faio::fs {
/** @brief
 * 递归删除资源上限；深度包含根目录，batch_size限制一个文件job的枚举步数。 */
struct recursive_remove_options {
  std::size_t max_depth{256};  ///< 同时打开的目录数上限，不递归消耗C++调用栈。
  std::size_t max_state_bytes{1024 * 1024};  ///< 根路径及持有名称的总内存上限。
  std::size_t batch_size{64};  ///< 每个job的最大遍历步数；上限4096。
};
}
