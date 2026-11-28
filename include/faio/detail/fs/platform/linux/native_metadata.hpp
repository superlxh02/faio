/**
 * @file faio/detail/fs/platform/linux/native_metadata.hpp
 * @brief 原生异步元数据请求。
 */
#pragma once
#include "faio/detail/fs/metadata.hpp"
#include "faio/detail/io/awaiter/native_file.hpp"
#include <fcntl.h>

namespace faio::fs::detail {
#if defined(__linux__)
/**
 *  @brief 原生 STATX 的类型化结果适配，无需子协程、服务任务或辅助线程。
 * @details 等待器自身持有输出结构，提交前移动不会遗留旧地址；仅在 await_suspend 中
 *          将固定的输出地址写入请求，直到最终 CQE 完成且取消排空前，该地址均保持有效。
 */
class native_metadata_awaiter : public io::detail::io_registrant_awaiter<native_metadata_awaiter> {
  using base_type = io::detail::io_registrant_awaiter<
      native_metadata_awaiter>; /**< 注册等待器的共用基类，复用提交、取消与完成握手。 */

 public:
  /** @brief 建立元数据请求并固定原生输出存储。 */
  native_metadata_awaiter(io::io_context context, int descriptor, std::string filename, int flags)
      : base_type{request(descriptor, std::move(filename), flags), std::move(context)} {}

  /** @brief 转移等待器状态和资源租约。 */
  native_metadata_awaiter(native_metadata_awaiter&&) noexcept = default;

  /** @brief 禁止复制此对象。 */
  native_metadata_awaiter(const native_metadata_awaiter&) = delete;

  /**  @brief 在最终协程帧内确定 STATX 输出地址后，直接交给原生请求桥接流程。 */
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    request_.buffer = &attributes_;
    return base_type::await_suspend(continuation);
  }

  /**  @brief CQE 完成后，内核已不再借用输出内存；直接返回该快照，无需重新查询文件系统。 */
  expected<file_metadata> await_resume() const noexcept {
    if (_user_data.result < 0)
      return std::unexpected{
          ::faio::error{static_cast<int>(-_user_data.result), _user_data.transferred}};
    return file_metadata{attributes_};
  }

 private:
  /** @brief 返回元数据等待器内部的原生请求。 */
  static io::detail::io_request request(int descriptor, std::string filename, int flags) {
    io::detail::io_request value;
    value.kind = io::detail::operation_kind::STATX;
    value.bypass_resource_registration = true;
    value.fd = descriptor;
    value.path = std::move(filename);
    value.flags = flags;
    value.argument = STATX_BASIC_STATS;
    return value;
  }

  struct statx attributes_{}; /**< CQE 完成前唯一的输出位置，其内存不随 SQ/CQ 节点移动。 */
};
#endif
}  // namespace faio::fs::detail
