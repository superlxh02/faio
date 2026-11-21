/**
 * @file faio/detail/io/core/operation_state.hpp
 * @brief 稳定操作槽与完成链。
 */
#pragma once
#include "faio/detail/common/move_only_function.hpp"
#include "faio/detail/io/protocol/request.hpp"

namespace faio::io::detail {
/** @brief 无需挂起的即时操作结果。 */
struct immediate_result {
  std::int64_t result{};       /**< 已完成操作的返回值；具体错误解释遵循所属接口约定。 */
  std::uint64_t transferred{}; /**< 操作已完成的字节数，失败或取消时仍保留有效进度。 */
};

/**  @brief 地址稳定的池化操作；全部普通字段由 domain 锁串行保护。 */
struct operation_state {
  io_request request;       /**< 稳定存放的统一 IO 请求，内核完成前保留参数与资源租约。 */
  completion_target target; /**< 单个指针指向的完成节点，安全完成后仅发布一次。 */
  ::faio::move_only_function<void(int)>
      control_completion;  /**< 负责孤立 close 请求的 CQE 回调，并持有回调自身的状态。 */
  operation_token token{}; /**< 包含槽位和代际的操作身份，防止取消或完成事件命中过期操作。 */
  std::uint32_t slot{}, generation{}; /**< 槽位当前代际，阻止迟到完成或取消命中复用后的操作。 */
  std::int64_t result{};              /**< 已完成操作的返回值；具体错误解释遵循所属接口约定。 */
  std::uint64_t transferred{};        /**< 操作已完成的字节数，失败或取消时仍保留有效进度。 */
  int cancellation{};                 /**< 首次记录的取消或超时错误码，后续事件不得覆盖。 */
  bool counts_active{}, allocated{}, accepted{}, terminal{}, running_file{}, connecting{},
      uses_reader{}, uses_writer{}, observer{},
      queued_completion{}; /**< 完成节点是否已链接到待发布队列，避免重复链接。 */
  bool native_inflight{}, native_pending{}, native_cancel_requested{}, native_waiting_release{},
      native_close{};      /**< 本操作是否承担原生关闭句柄的唯一责任。 */
  operation_state* next{}; /**< 内嵌完成节点；发布完成时无需分配内存。 */
  operation_state*
      completed_previous{};         /**< 双向完成链允许以 O(1) 复杂度领取当前即时完成的结果。 */
  operation_state* observer_next{}; /**< 同一资源就绪观察者链表中的后继，不拥有后继节点。 */
};

}  // namespace faio::io::detail
