/**
 * @file faio/detail/io/awaiter/native_file.hpp
 * @brief 原生文件请求与关闭兜底。
 */
#pragma once
#include "faio/detail/blocking/call.hpp"
#include "faio/detail/io/awaiter/request.hpp"
#include "faio/detail/io/platform/native_operations.hpp"

namespace faio::io {
/**  @brief 查询固定后端支持的原生 opcode；不启动文件线程池或系统调用。 */
inline bool supports_native(const io_context& context, detail::operation_kind kind) noexcept {
  return context && context.domain()->supports_native(kind);
}

/**
 * @brief 文件 provider 的原生桥接；外层 File 活跃租约保证原 fd 和负载在操作期间有效。
 * @details 裸 fd 不另行注册资源所有者；地址稳定的操作持有路径和 iovec 描述符。
 * cancellable=false 用于已接管的原生 close；最终 CQE 到达前不得恢复协程或复用 fd。
 */
class native_file_awaiter : public detail::IORegistrantAwaiter<native_file_awaiter> {
 public:
  /** @brief 构造原生文件请求等待器。 */
  native_file_awaiter(io_context context, detail::io_request request, bool cancellable)
      : IORegistrantAwaiter{
            mark_borrowed(std::move(request), cancellable), std::move(context), cancellable} {}

  /**
   *  @brief 读取已发布的结果，同时保留负 errno 与实际完成的进度。
   * @details 调用前原生借用已经排空；这里不重试 syscall 或再生成关闭请求。
   */
  expected<std::int64_t> await_resume() const noexcept {
    if (_user_data.result < 0)
      return std::unexpected{
          detail::decode_io_error(static_cast<int>(-_user_data.result), _user_data.transferred)};
    return _user_data.result;
  }

 private:
  /** @brief 外层 File lease 已固定 fd，避免 raw fd 桥再建立第二个资源拥有者。 */
  static detail::io_request mark_borrowed(detail::io_request request, bool cancellable) {
    request.bypass_resource_registration = true;
    request.uncancellable = !cancellable;
    return request;
  }
};

/**  @brief 原生普通文件请求直接返回 awaiter，无需创建转发用的子协程。 */
inline native_file_awaiter native_file_op(io_context context,
                                          detail::io_request request,
                                          bool cancellable = true) {
  return native_file_awaiter{std::move(context), std::move(request), cancellable};
}

/**
 * @brief 直接等待原生 CLOSE；仅在准备前失败时接管兜底关闭责任。
 * @details 正常原生路径不创建子任务，也不调用文件执行器或清理执行器。
 */
class native_file_close_awaiter {
  /** @brief 文件句柄关闭任务。 */
  struct close_work {
    detail::native_descriptor
        fd; /**< 仅在准备前失败时承担兜底关闭，不能与已接受的原生 CLOSE 并行。 */

    expected<std::int64_t> operator()() const noexcept {
      return detail::native_operations::close_file_cleanup(fd);
    }
  };

 public:
  /** @brief 构造原生文件关闭等待器。 */
  native_file_close_awaiter(io_context context, detail::io_request request)
      : fd_(request.fd),
        native_(context, std::move(request), false),
        context_(std::move(context)) {}

  /** @brief 禁止复制构造原生文件关闭等待器。 */
  native_file_close_awaiter(const native_file_close_awaiter&) = delete;

  /** @brief 移动构造原生文件关闭等待器。 */
  native_file_close_awaiter(native_file_close_awaiter&&) = default;

  /** @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。 */
  bool await_ready() const noexcept { return false; }

  /**
   *  @brief 优先使用原生 CLOSE；只有在稳定槽准备前失败，才接管兜底责任。
   * @param continuation 等待关闭完成的原协程，不创建用于转发的子任务。
   * @return 按实际选择的 awaiter 返回挂起结果，接受成功的 CLOSE 绝不重复执行。
   */
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    const bool suspended = native_.await_suspend(continuation);
    if (suspended || native_.was_prepared() || fd_ < 0)
      return suspended;
    fallback_.emplace(context_, close_work{fd_}, context_.cleanup(), false);
    return fallback_->await_suspend(continuation);
  }

  /** @brief 只读取曾被选择的完成来源，不再执行或重试任何 close。 */
  expected<std::int64_t> await_resume() {
    if (fallback_)
      return fallback_->await_resume();
    return native_.await_resume();
  }

 private:
  detail::native_descriptor fd_; /**< 移动请求前保存唯一的关闭对象；正常原生路径不复制请求。 */
  native_file_awaiter
      native_;         /**< 桥接普通原生提交与真实 CQE 的发布，保留稳定 token 的生命周期。 */
  io_context context_; /**< 关闭服务租约，不依赖恢复时所在 worker 的 TLS。 */
  std::optional<blocking::call_awaiter<close_work>>
      fallback_; /**< 仅准备前失败才构造；正常原生请求不构造此对象。 */
};

/** @brief 构造原生文件关闭操作。 */
inline native_file_close_awaiter native_file_close(io_context context, detail::io_request request) {
  return native_file_close_awaiter{std::move(context), std::move(request)};
}
}  // namespace faio::io
