/**
 * @file faio/detail/fs/core/file_cursor.hpp
 * @brief 文件与目录异步接口：共享文件游标。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include "faio/detail/sync/mutex.hpp"
#include <cstdint>

namespace faio::fs::detail {
/** @brief clone 共享的逻辑游标；异步锁跨 syscall 保持串行，不阻塞 worker。 */
struct cursor_state {
  sync::mutex lane;  ///< 串行化同一文件共享游标的异步互斥锁。
  std::uint64_t position{};  ///< 当前逻辑游标的字节偏移；更新受所属游标锁保护。
};
}
