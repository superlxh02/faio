/**
 * @file faio/detail/fs/core/file_cursor.hpp
 * @brief 共享文件游标。
 */
#pragma once
#include "faio/detail/sync/mutex.hpp"
#include <cstdint>

namespace faio::fs::detail {
/**  @brief clone 共享的逻辑游标；异步锁将系统调用串行化，不阻塞工作线程。 */
struct cursor_state {
  sync::mutex lane;         /**< 串行化同一文件共享游标的异步互斥锁。 */
  std::uint64_t position{}; /**< 当前逻辑游标的字节偏移；更新受所属游标锁保护。 */
};
}  // namespace faio::fs::detail
