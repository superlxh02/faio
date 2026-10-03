#pragma once
#include "faio/detail/execution/execute.hpp"
#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io {
/** @brief 查询固定后端支持的原生 opcode；不启动文件线程池或系统调用。 */
inline bool supports_native(const io_context& context, detail::operation_kind kind) noexcept {
  return context && context.domain()->supports_native(kind);
}

/** @brief 文件 provider 的原生桥；外层 File active lease 覆盖原 fd 与 payload
 * 生命周期。
 * @details raw fd 不另注册资源拥有者；稳定 operation owns 路径和 iovec 描述符。
 *          cancellable=false 用于已接管的原生 close，最终 CQE 前不能恢复或复用
 * fd。
 */
class native_file_awaiter : public detail::IORegistrantAwaiter<native_file_awaiter> {
 public:
  native_file_awaiter(io_context context, detail::io_request request, bool cancellable)
      : IORegistrantAwaiter{
            mark_borrowed(std::move(request), cancellable), std::move(context), cancellable} {}

  /** @brief 读取统一发布结果，负 errno 与真实已完成进度一起保留。
   * @details 调用前原生借用已经排空；这里不重试 syscall 或再生成关闭请求。
   */
  expected<std::int64_t> await_resume() const noexcept {
    if (_user_data.result < 0)
      return std::unexpected{
          detail::decode_io_error(static_cast<int>(-_user_data.result),
                                  _user_data.transferred)};  // 取消不抹掉已经传输或删除的进度。
    return _user_data.result;
  }

 private:
  /// @brief 外层 File lease 已固定 fd，避免 raw fd 桥再建立第二个资源拥有者。
  static detail::io_request mark_borrowed(detail::io_request request, bool cancellable) {
    request.bypass_resource_registration =
        true;  // 路径/向量仍随请求进稳定槽，只有 fd 生命周期由 File lease 管理。
    request.uncancellable = !cancellable;  // 已接管 CLOSE 不允许先取消返回并复用尚未关闭的 fd。
    return request;
  }
};

/** @brief 原生普通文件请求直接返回 awaiter，不创建用于转发的子协程。 */
inline native_file_awaiter native_file_op(io_context context,
                                          detail::io_request request,
                                          bool cancellable = true) {
  return native_file_awaiter{std::move(context), std::move(request), cancellable};
}

/** @brief 原生CLOSE的直接awaiter；只在准备前失败时接管兜底关闭责任。
 * @details 正常原生路径不创建child task，不调用文件/清理执行器。
 */
class native_file_close_awaiter {
  struct close_work {
    detail::native_descriptor
        fd;  ///< 仅准备前失败的兜底关闭责任，绝不与已接受的 native CLOSE 并行。

    expected<std::int64_t> operator()() const noexcept {
      // close 只调用一次；EINTR 后数字 fd
      // 可能已释放，重试会关闭后来复用的句柄。
#if defined(_WIN32)
      if (!::CloseHandle(reinterpret_cast<HANDLE>(fd)))
        return std::unexpected{windows::make_windows_error(::GetLastError())};
#else
      if (::close(fd) < 0)
        return std::unexpected{make_error(errno)};
#endif
      return std::int64_t{0};
    }
  };

 public:
  native_file_close_awaiter(io_context context, detail::io_request request)
      : fd_(request.fd),
        native_(context, std::move(request), false),
        context_(std::move(context)) {}

  native_file_close_awaiter(const native_file_close_awaiter&) = delete;

  native_file_close_awaiter(native_file_close_awaiter&&) = default;

  bool await_ready() const noexcept { return false; }

  /** @brief 优先原生 CLOSE；只有从未准备稳定槽的失败才接管兜底责任。
   * @param continuation 等待关闭完成的原协程，不创建用于转发的子任务。
   * @return 按实际选择的 awaiter 返回挂起结果，接受成功的 CLOSE 绝不重复执行。
   */
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    const bool suspended = native_.await_suspend(continuation);
    if (suspended || native_.was_prepared() || fd_ < 0)
      return suspended;  // 已接受或已完成的CLOSE结果绝不重做。
    // 正常 native 路径不会到此；准备前失败也必须保证唯一 fd 清理责任被接管。
    fallback_.emplace(context_, close_work{fd_}, context_.cleanup(), false);
    return fallback_->await_suspend(continuation);  // 仅无context/准备前失败兜底。
  }

  /// @brief 只读取曾被选择的完成来源，不再执行或重试任何 close。
  expected<std::int64_t> await_resume() {
    if (fallback_)
      return fallback_->await_resume();
    return native_.await_resume();
  }

 private:
  detail::native_descriptor fd_;  ///< 移动请求前保存唯一关闭对象；正常native路径不复制请求。
  native_file_awaiter native_;    ///< 普通原生提交与真实 CQE 发布桥，保留稳定 token 生命周期。
  io_context context_;            ///< 关闭服务租约，不依赖恢复时所在 worker 的 TLS。
  std::optional<execution::execute_awaiter<close_work>>
      fallback_;  ///< 仅准备前失败才构造；正常原生请求保持 disengaged。
};

inline native_file_close_awaiter native_file_close(io_context context, detail::io_request request) {
  return native_file_close_awaiter{std::move(context), std::move(request)};
}
}  // namespace faio::io
