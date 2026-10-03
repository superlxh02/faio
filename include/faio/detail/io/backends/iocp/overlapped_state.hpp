#pragma once
#include "faio/detail/io/platform/windows_handle.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace faio::io::windows {
/// @brief Windows 原生请求的框架分类；列出类型不表示该 opcode 已完成适配。
enum class operation_kind : std::uint8_t {
  read,
  write,
  receive,
  send,
  receive_from,
  send_to,
  accept,
  connect,
  readiness
};
/** @brief 稳定 OVERLAPPED 的拟定生命周期，取消中仍不得回收原请求。
 * @details quiescent 必须证明原完成包已消费；published 后才允许退休身份。
 */
enum class lifecycle : std::uint8_t {
  prepared,
  accepted,
  in_flight,
  cancelling,
  quiescent,
  published,
  retired
};
/// @brief 业务完成、控制 ACK 和借用释放分别表达，不能都作为协程完成包。
enum class native_completion_kind : std::uint8_t {
  operation_complete,
  control_ack,
  buffer_released
};
/** @brief 稳定 OVERLAPPED 存储协议，取消 ACK 不能使此状态提前释放。
 * @details 实际实现必须保留状态和 payload lease 直到原始操作的完成包已消费。
 * synchronous success 默认也等待完成包；启用 SKIP_COMPLETION_PORT_ON_SUCCESS 时
 * 由 backend 明确合成唯一结果，不能既合成又消费同一操作的真实完成。
 */
struct overlapped_operation_state {
#if defined(_WIN32)
  OVERLAPPED native{};             ///< 必须地址稳定，直到内核最后一次引用解除。
  std::array<WSABUF, 2> vectors{}; ///< 原生 buffer 描述由状态拥有。
#else
  alignas(void *)
      std::array<std::byte, sizeof(std::uintptr_t) *
                                4> native{}; ///< 非 Windows 仅提供结构 smoke
                                             ///< 存储，不能提交给 IOCP。
#endif
  std::uint64_t token{}; ///< slot/generation，后端解码前检查代际。
  operation_kind
      kind{}; ///< 与未来对应原生 API 的请求种类，完成时不能用 fd 猜测。
  std::atomic<lifecycle> phase{
      lifecycle::prepared}; ///< 预留跨线程状态发布，当前框架不执行内核操作。
  void *payload{}; ///< 将来由独占/不可变 buffer lease 固定，不借用可移动
                   ///< awaiter。
  std::size_t
      payload_size{}; ///< 受 lease 固定的字节范围，不代表内核已完成的传输量。
  std::uint32_t
      native_error{}; ///< 保留 Windows 错误域，不能错误地解释为 POSIX errno。
  std::uint64_t
      transferred{}; ///< 原生完成的真实进度，取消不能先抹去已经传输的字节。
  bool
      skip_completion_on_success{}; ///< 未来显式配置时才合成成功，绝不同时消费同一成功的完成包。
};
/** @brief 未来 IOCP 消费包到中立协议的字段约定，不包含 reactor 或线程池模拟。
 */
struct native_completion {
  std::uint64_t token{}; ///< 原请求完整代际，查找前拒绝迟到或已退休身份。
  std::int64_t result{}; ///< 保留真实原生结果；负值只能按 Windows 错误域解释。
  native_completion_kind
      kind{}; ///< 角色决定是否允许业务发布或仅解除控制/借用责任。
};
} // namespace faio::io::windows
