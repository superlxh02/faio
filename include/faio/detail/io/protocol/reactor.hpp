/**
 * @file faio/detail/io/protocol/reactor.hpp
 * @brief 就绪反应器。
 */
#pragma once
#include <cstdint>

namespace faio::io::detail {
/** @brief 后端中立的方向就绪与关闭信息；ERR/EOF 仍须由真实 IO 判断终态。 */
enum readiness_bits : std::uint32_t {
  READABLE_BIT = 1,
  WRITABLE_BIT = 2,
  ERROR_BIT = 4,
  READ_CLOSED_BIT = 8,
  WRITE_CLOSED_BIT = 16,
  CLOSED_BIT = READ_CLOSED_BIT | WRITE_CLOSED_BIT
};

/**
 * @brief 内核只保存注册代际标识，不保存协程或 awaiter 地址。
 *  @details reactor 完整填写返回 count 内的 key/flags，未使用输出槽无需清零。
 *          调用方只读取有效数量；需要空事件时显式使用 readiness_event{}。
 */
struct readiness_event {
  std::uint64_t key;   /**< 单调资源身份，零值保留给控制唤醒事件。 */
  std::uint32_t flags; /**< 本次事件的就绪与关闭标志。 */
};

}  // namespace faio::io::detail
