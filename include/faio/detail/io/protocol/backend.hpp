/**
 * @file faio/detail/io/protocol/backend.hpp
 * @brief 跨平台异步 IO：后端封装与提交协议。
 * @details 定义后端中立的请求与事件形状；资源键用于识别长期资源，操作令牌用于核对槽位代际，消费者地址仅由用户态完成发布使用。
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace faio::io::detail {
/** @brief 原生注册值不截断 HANDLE/SOCKET；只有 POSIX adapter 转为 int。 */
enum class native_handle_kind : std::uint8_t { posix_descriptor, windows_handle, windows_socket };

struct native_registration {
  std::uintptr_t value{};  ///< 保存完整原生值；不得把 HANDLE 或 Win64 SOCKET 缩窄为 int。
  native_handle_kind kind{native_handle_kind::posix_descriptor};  ///< 拒绝平台种类不匹配的注册。
};

/** @brief 全部后端共用的接受协议；would_queue 尚未保存内核引用。 */
enum class backend_submit_status : std::uint8_t { accepted, would_queue, rejected };

struct backend_submit_result {
  backend_submit_status status{backend_submit_status::rejected};  ///< 后端提交结果：已接受、暂满或明确拒绝。
  int error{};  ///< 正 errno；仅 rejected 使用。
};

/** @brief 原生完成、取消确认与 buffer release 是不同事件，不能混为一次恢复。 */
enum class backend_event_kind : std::uint8_t { readiness, result, cancel_ack, buffer_release };

/** @details poll 只完整填写返回 count 内的记录；默认构造输出槽不清零。
 *          消费者不得读取 count 外的记录；需要空事件时显式使用
 * backend_event{}。
 */
struct backend_event {
  backend_event_kind kind;  ///< 当前请求、事件或句柄的种类标签。
  std::uint64_t key;  ///< readiness 为资源代际，其余为操作代际；零为控制通道。
  std::int64_t result;  ///< 结果为字节数/原生结果，失败为负错误码，ACK 不携带业务完成。
  std::uint32_t flags;  ///< 中立 readiness 位或原生完成通知标记。
};

/** @brief 请求描述符指向稳定槽中的拥有型请求，绝不指向 awaiter。 */
struct backend_operation {
  std::uint64_t token{};  ///< 完整 slot/generation，控制 CQE 不可借用 token 高位做 tag。
  void* request{};  ///< 指向域拥有的稳定 typed 请求；accepted 后最后事件前不得移动或销毁。

};

struct backend_flush_result {
  std::size_t submitted{};  ///< 本轮真正交给内核的 SQE 数，允许 partial flush。
  bool pending{};  ///< SQ 或取消控制队列仍有待提交责任，驱动器必须继续推进。
  int error{};  ///< 正错误码，永久失败不能被忽略；可恢复背压由后端保留并重试。
};

/** @brief 可验证的后端工作量快照；原生 CQE 与取消确认分别计数。 */
struct backend_statistics {
  std::uint64_t native_submitted{},  ///< 已被原生后端接受的业务请求累计数量。
      native_completed{},  ///< 已经收到业务终态的原生请求累计数量。
      cancel_ack{},  ///< 收到的原生取消控制确认数量，不代表业务负载已释放。
      buffer_notifications{};  ///< 收到的零拷贝负载释放通知数量。
  std::uint64_t native_flushed{};  ///< io_uring_submit 实際接受的 SQE 总数，包含控制取消。
};

}
