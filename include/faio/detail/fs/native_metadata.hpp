#pragma once
#include "faio/detail/fs/metadata.hpp"
#include "faio/detail/io/native_file_op.hpp"
#include <fcntl.h>

namespace faio::fs::detail {
#if defined(__linux__)
/** @brief 原生STATX的类型化结果桥；没有子协程、服务job或线程辅助。
 * @details awaiter自身持有输出结构，提交前移动不会遗留旧地址；只有await_suspend
 *          将稳定输出地址写入请求，CQE最终完成及取消排空前该地址不会失效。
 */
class native_metadata_awaiter
    : public io::detail::IORegistrantAwaiter<native_metadata_awaiter> {
  using Base = io::detail::IORegistrantAwaiter<native_metadata_awaiter>;

public:
  native_metadata_awaiter(io::io_context context, int descriptor,
                          std::string filename, int flags)
      : Base{request(descriptor, std::move(filename), flags),
             std::move(context)} {}
  native_metadata_awaiter(native_metadata_awaiter &&) noexcept = default;
  native_metadata_awaiter(const native_metadata_awaiter &) = delete;
  /** @brief 在最终协程帧内固定STATX输出地址后，直接进入统一原生请求桥。 */
  bool await_suspend(std::coroutine_handle<> continuation) noexcept {
    request_.buffer = &attributes_; // 此时awaiter已经位于最终等待它的协程帧。
    return Base::await_suspend(continuation); // 直接准备SQE及等待原生CQE。
  }
  /** @brief CQE已经解除内核输出借用；返回该快照，不重新查询文件系统。 */
  expected<Metadata> await_resume() const noexcept {
    if (_user_data.result < 0)
      return std::unexpected{
          Error{static_cast<int>(-_user_data.result),
                _user_data.transferred}}; // 原生错误及取消仲裁由同一桥填充。
    return Metadata{attributes_}; // 仅转换已完成结果，不进行额外系统调用。
  }

private:
  static io::detail::io_request request(int descriptor, std::string filename,
                                        int flags) {
    io::detail::io_request value;
    value.kind =
        io::detail::operation_kind::statx; // 后端直接使用io_uring_prep_statx。
    value.bypass_resource_registration =
        true;              // File lease或路径请求自行持有句柄。
    value.fd = descriptor; // 空路径配合AT_EMPTY_PATH时查询外层File租约的句柄。
    value.path =
        std::move(filename); // 路径由稳定请求拥有，内核不借用临时字符串。
    value.flags = flags;     // 保留调用方的follow/no-follow及fd-relative语义。
    value.argument =
        STATX_BASIC_STATS; // 明确请求公开Metadata使用的基础属性集合。
    return value;
  }
  struct statx
      attributes_{}; ///< CQE前唯一输出位置；输出内存不随SQ/CQ节点移动。
};
#endif
} // namespace faio::fs::detail
