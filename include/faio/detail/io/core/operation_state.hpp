/**
 * @file faio/detail/io/core/operation_state.hpp
 * @brief 跨平台异步 IO：稳定操作槽与完成链。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/common/move_only_function.hpp"
#include "faio/detail/io/protocol/request.hpp"

namespace faio::io::detail {
struct immediate_result {
  std::int64_t result{};  ///< 已完成操作的返回值；具体错误解释遵循所属接口约定。
  std::uint64_t transferred{};  ///< 操作已完成的字节数，失败或取消时仍保留有效进度。
};

/** @brief 地址稳定的池化操作；全部普通字段由 domain 锁串行保护。 */
struct operation_state {
  io_request request;  ///< 稳定存放的统一 IO 请求，内核完成前保留参数与资源租约。
  completion_target target;  ///< 完成消费者及其静态发布函数，安全完成后仅调用一次。
  ::faio::move_only_function<void(int)> control_completion;  ///< 孤立close的拥有型CQE回调。
  operation_token token{};  ///< 包含槽位与代际的操作身份，避免取消或完成命中过期操作。
  std::uint32_t slot{},  ///< 操作池槽位编号，与 generation 一起组成稳定取消令牌。
      generation{};  ///< 槽位当前代际，阻止迟到完成或取消命中复用后的操作。
  std::int64_t result{};  ///< 已完成操作的返回值；具体错误解释遵循所属接口约定。
  std::uint64_t transferred{};  ///< 操作已完成的字节数，失败或取消时仍保留有效进度。
  int cancellation{};  ///< 首次记录的取消或超时错误码，后续事件不得覆盖。
  bool counts_active{},  ///< 该操作是否已计入资源活跃借用，回收时成对归还。
      allocated{},  ///< 槽位是否已被分配，防止使用空闲或过时代际。
      accepted{},  ///< 域是否已接管操作完成责任，拒绝路径不设置该标志。
      terminal{},  ///< 操作终态是否已经建立，防止完成与取消重复仲裁。
      running_file{},  ///< 阻塞文件调用是否仍在执行，结束前不能释放借用缓冲区。
      connecting{},  ///< 连接是否已进入等待完成阶段，重试时需查询真实 socket 错误。
      uses_reader{},  ///< 本操作是否持有资源的读方向执行权。
      uses_writer{},  ///< 本操作是否持有资源的写方向执行权。
      observer{},  ///< 本操作是否只观察就绪，区别于消费数据的读写操作。
      queued_completion{};  ///< 完成节点是否已链接到待发布队列，避免重复链接。
  bool native_inflight{},  ///< 原生操作是否已被后端接受且尚未安全完成。
      native_pending{},  ///< 原生请求是否因后端容量不足留在待提交队列。
      native_cancel_requested{},  ///< 原生取消是否已经发出，避免重复占用控制提交容量。
      native_waiting_release{},  ///< 业务完成后是否仍等待内核解除负载借用。
      native_close{};  ///< 本操作是否承担原生关闭句柄的唯一责任。
  operation_state* next{};  ///< 内嵌完成节点；发布完成时无需分配内存。
  operation_state* completed_previous{};  ///< 双向完成链允许 O(1) 领取当前即时完成。
  operation_state* observer_next{};  ///< 同一资源就绪观察者链表中的后继，不拥有后继节点。
};

}
