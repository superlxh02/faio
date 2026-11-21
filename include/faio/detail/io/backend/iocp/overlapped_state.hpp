#pragma once
/**
 * @file faio/detail/io/backend/iocp/overlapped_state.hpp
 * @brief IOCP 实际使用的稳定原生请求前缀。
 */
#include "faio/detail/io/platform/windows/handle.hpp"
#include <array>
#include <cstddef>
#include <type_traits>

namespace faio::io::windows {
/**
 * @brief 标准布局的原生请求前缀，不含协程、awaiter 或用户 continuation。
 * @details OVERLAPPED 必须是首成员，完成消费者可从内核返回的地址恢复此前缀。
 *          owner 指向后端池化节点；节点的地址在请求接受后至最终完成包到达前保持不变。
 *          token、请求租约和取消状态由后端节点/公共域独立维护，避免双重终态。
 */
template <class Owner>
struct overlapped_operation_state {
  static_assert(!std::is_void_v<Owner>, "OVERLAPPED owner must retain its concrete type");
#if defined(_WIN32)
  OVERLAPPED overlapped{};
#else
  alignas(void*) std::array<std::byte, sizeof(void*) * 4> overlapped{};
#endif
  Owner* owner{}; /**< 纯用户态池节点，不指向协程帧或可移动包装对象。 */
};

}  // namespace faio::io::windows
