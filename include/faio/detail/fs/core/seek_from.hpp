/**
 * @file faio/detail/fs/core/seek_from.hpp
 * @brief 文件定位基准。
 */
#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace faio::fs {
/**  @brief 逻辑游标基准；位移采用有符号值以表达 current/end 的负向移动。 */
struct seek_from {
  /** @brief 文件游标偏移的解释基准。 */
  enum class origin { start, current, end };

  origin base{origin::start}; /**< 偏移解释的起点：文件开头、当前游标或文件末尾。 */
  std::int64_t offset{};      /**< 本次文件操作的字节偏移，定位基准由对应接口约定。 */

  /** @brief 构造相对文件开头的偏移，超出有符号范围时抛出异常。 */
  static seek_from start(std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      throw std::out_of_range("文件偏移超过平台范围");
    return {origin::start, static_cast<std::int64_t>(offset)};
  }

  /** @brief 构造相对当前文件游标的有符号偏移。 */
  static seek_from current(std::int64_t offset) noexcept { return {origin::current, offset}; }

  /** @brief 构造相对文件末尾的有符号偏移。 */
  static seek_from end(std::int64_t offset) noexcept { return {origin::end, offset}; }
};
}  // namespace faio::fs
