#pragma once
/** @file overlapped_state.hpp @brief IOCP 实际使用的稳定原生请求前缀。 */
#include "faio/detail/io/platform/windows_handle.hpp"
#include <array>
#include <cstddef>
#include <type_traits>

namespace faio::io::windows {
/**
 * @brief 标准布局的原生请求前缀，不含协程、awaiter 或用户 continuation。
 * @details OVERLAPPED 必须是首成员，完成消费者可从内核返回的地址恢复此前缀。
 *          owner 指向后端池化节点；节点的地址在接受到最终完成包期间固定。
 *          token、请求租约和取消状态由后端节点/公共域独立维护，避免双重终态。
 */
struct overlapped_operation_state {
#if defined(_WIN32)
  OVERLAPPED overlapped{};  ///< 内核访问的原生状态，最终完成包消费后才能重用。
#else
  alignas(void*) std::array<std::byte, sizeof(void*) * 4> overlapped{};
#endif
  void* owner{};  ///< 纯用户态池节点，不指向协程帧或可移动包装对象。
};

static_assert(std::is_standard_layout_v<overlapped_operation_state>);
static_assert(offsetof(overlapped_operation_state, overlapped) == 0);
}  // namespace faio::io::windows
