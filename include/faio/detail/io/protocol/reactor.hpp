/**
 * @file faio/detail/io/protocol/reactor.hpp
 * @brief 跨平台异步 IO：就绪反应器。
 * @details 定义后端中立的请求与事件形状；资源键用于识别长期资源，操作令牌用于核对槽位代际，消费者地址仅由用户态完成发布使用。
 */
#pragma once
#include <cstdint>

namespace faio::io::detail {
/// @brief 后端中立的方向就绪与关闭信息；ERR/EOF 仍须由真实 IO 判断终态。
enum readiness_bits : std::uint32_t {
  readable_bit = 1,
  writable_bit = 2,
  error_bit = 4,
  /// @brief 读方向 EOF；保留既有读关闭位值，可能仍有未读数据。
  read_closed_bit = 8,
  /// @brief 写方向关闭；不能使单独的 readable 观察立即完成。
  write_closed_bit = 16,
  /// @brief 聚合信息掩码，任一方向关闭即匹配。
  closed_bit = read_closed_bit | write_closed_bit
};

/// @brief 内核只保存注册代际标识，不保存协程或 awaiter 地址。
/** @details reactor 完整填写返回 count 内的 key/flags，未使用输出槽无需清零。
 *          调用方只读取有效数量；需要空事件时显式使用 readiness_event{}。
 */
struct readiness_event {
  std::uint64_t key;  ///< 单调资源身份，零值保留给控制唤醒事件。
  std::uint32_t flags;  ///< 本次原生操作的标志位，按对应系统调用语义解释。
};

}
