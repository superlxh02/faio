/**
 * @file faio/detail/fs/platform/linux/native_metadata.hpp
 * @brief 文件与目录异步接口：原生异步元数据请求。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include "faio/detail/fs/metadata.hpp"
#include "faio/detail/io/awaiter/native_file.hpp"
#include <fcntl.h>

namespace faio::fs::detail {
#if defined(__linux__)
/** @brief 原生STATX的类型化结果桥；没有子协程、服务job或线程辅助。
 * @details awaiter自身持有输出结构，提交前移动不会遗留旧地址；只有await_suspend
 *          将稳定输出地址写入请求，CQE最终完成及取消排空前该地址不会失效。
 */
class native_metadata_awaiter : public io::detail::IORegistrantAwaiter<native_metadata_awaiter> {
  using Base = io::detail::IORegistrantAwaiter<native_metadata_awaiter>;  ///< 统一注册等待器基类，复用提交、取消与完成握手。

 public:
  native_metadata_awaiter(io::io_context context, int descriptor, std::string filename, int flags)
      : Base{request(descriptor, std::move(filename), flags), std::move(context)} {}

  native_metadata_awaiter(native_metadata_awaiter&&) noexcept = default;

  native_metadata_awaiter(const native_metadata_awaiter&) = delete;

  /** @brief 在最终协程帧内固定STATX输出地址后，直接进入统一原生请求桥。 */
  bool await_suspend(std::coroutine_handle<> continuation) noexcept {
    /// @brief 此时awaiter已经位于最终等待它的协程帧。
    request_.buffer = &attributes_;
    /// @brief 直接准备SQE及等待原生CQE。
    return Base::await_suspend(continuation);
  }

  /** @brief CQE已经解除内核输出借用；返回该快照，不重新查询文件系统。 */
  expected<Metadata> await_resume() const noexcept {
    /// @brief 原生错误及取消仲裁由同一桥填充。
    if (_user_data.result < 0)
      return std::unexpected{Error{static_cast<int>(-_user_data.result),
                                   _user_data.transferred}};
    /// @brief 仅转换已完成结果，不进行额外系统调用。
    return Metadata{attributes_};
  }

 private:
  static io::detail::io_request request(int descriptor, std::string filename, int flags) {
    io::detail::io_request value;
    /// @brief 后端直接使用io_uring_prep_statx。
    value.kind = io::detail::operation_kind::statx;
    /// @brief File lease或路径请求自行持有句柄。
    value.bypass_resource_registration = true;
    /// @brief 空路径配合AT_EMPTY_PATH时查询外层File租约的句柄。
    value.fd = descriptor;
    /// @brief 路径由稳定请求拥有，内核不借用临时字符串。
    value.path = std::move(filename);
    /// @brief 保留调用方的follow/no-follow及fd-relative语义。
    value.flags = flags;
    /// @brief 明确请求公开Metadata使用的基础属性集合。
    value.argument = STATX_BASIC_STATS;
    return value;
  }

  struct statx attributes_{};  ///< CQE前唯一输出位置；输出内存不随SQ/CQ节点移动。
};
#endif
}
