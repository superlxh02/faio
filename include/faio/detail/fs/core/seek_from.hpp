/**
 * @file faio/detail/fs/core/seek_from.hpp
 * @brief 文件与目录异步接口：文件定位基准。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace faio::fs {
/** @brief 逻辑游标基准；位移采用有符号值以表达 current/end 的负向移动。 */
struct seek_from {
  enum class origin { start, current, end };

  origin base{origin::start};  ///< 偏移解释的起点：文件开头、当前游标或文件末尾。
  std::int64_t offset{};  ///< 本次文件操作的字节偏移，定位基准由对应接口约定。

  /// @brief 把已连接的稳定操作交给其执行协议，之后只能交付一个终态。
  static seek_from start(std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      throw std::out_of_range("文件偏移超过平台范围");
    return {origin::start, static_cast<std::int64_t>(offset)};
  }

  /// @brief 取得当前线程已安装的上下文；缺少绑定时按接口约定返回空值。
  static seek_from current(std::int64_t offset) noexcept { return {origin::current, offset}; }

  static seek_from end(std::int64_t offset) noexcept { return {origin::end, offset}; }
};
}
