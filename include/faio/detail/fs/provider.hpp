#pragma once

#include "faio/detail/execution/execute.hpp"
#include "faio/detail/io/io.hpp"
#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <variant>

namespace faio::fs::detail {
/**
 * @brief 文件服务调用的结果转换，将void/整数结果统一为有符号expected。
 * @tparam F 拥有实际文件系统调用参数的可调用对象，仅fallback分支构造。
 */
template <class F>
struct file_syscall {
  F fallback;  ///< 仅fallback分支拥有该函数；原生分支不创建服务任务。

  expected<std::int64_t> operator()() {
    auto result = std::invoke(fallback);  // 只在文件/清理服务线程执行可能阻塞的syscall。
    if (!result)
      return std::unexpected{result.error()};  // 保留errno及取消时的真实部分进度。
    if constexpr (std::is_void_v<typename decltype(result)::value_type>)
      return std::int64_t{0};  // 成功的无返回值操作使用与原生CQE一致的零值。
    else
      return static_cast<std::int64_t>(*result);  // 字节数或已发生副作用的条数不能丢失。
  }
};

/** @brief 提交前选择一次的文件awaitable；原生分支直接使用IO协程桥。
 * @details 该对象没有协程帧，不创建原生请求之外的job或线程。variant只保存
 * 所选awaitable，结果仍由原始SQE/CQE完成；fallback只在内核无对应opcode时构造。
 */
template <class F>
class file_request_awaiter {
  using Service = execution::execute_awaiter<file_syscall<F>>;

 public:
  /** @brief 能力路由只在提交前选择一次，不以已接受请求的错误触发重做。
   * @param context 文件固定所属的服务租约，不读取恢复线程TLS。
   * @param request 拥有路径/iovec描述符的中立请求，fd由外层活跃租约保护。
   * @param fallback 仅原生opcode不可用时交给隔离服务的系统调用。
   * @param cancellable close等清理为false，业务借用IO为true且严格排空。
   */
  file_request_awaiter(io::io_context context,
                       io::detail::io_request request,
                       F fallback,
                       bool cancellable) {
    if (io::supports_native(context, request.kind)) {
      // 查实际内核能力，不按平台名字猜测。
      if (!cancellable && request.kind == io::detail::operation_kind::close)
        operation_.template emplace<2>(std::move(context),
                                       std::move(request));  // 关闭接管后不可被取消提前返回。
      else
        operation_.template emplace<1>(std::move(context),
                                       std::move(request),
                                       cancellable);  // 原始IO桥直接prep/submit及接收CQE。
    } else if (!cancellable) {
      // 关闭的fallback使用保留通道，普通队列已满也不能遗失关闭责任。
      auto service = context.cleanup();  // 保留清理容量，与普通文件服务分开。
      operation_.template emplace<3>(
          std::move(context), file_syscall<F>{std::move(fallback)}, std::move(service), false);
    } else {
      auto service = context.blocking();  // readiness文件IO或缺opcode操作才启动服务。
      operation_.template emplace<3>(
          std::move(context), file_syscall<F>{std::move(fallback)}, std::move(service), true);
    }
  }

  file_request_awaiter(const file_request_awaiter&) = delete;

  file_request_awaiter(file_request_awaiter&&) = default;

  bool await_ready() const noexcept { return false; }  // 构造和能力选择不执行文件IO。

  /** @brief 直接委托所选awaiter；不创建转发task或额外协程帧。 */
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    return std::visit(
        [continuation](auto& operation) -> bool {
          if constexpr (std::same_as<std::remove_cvref_t<decltype(operation)>, std::monostate>)
            return false;  // 防御空variant；await_resume将其转换为明确的EINVAL。
          else
            return operation.await_suspend(continuation);  // 原始gate决定即时完成或唯一挂起。
        },
        operation_);
  }

  /** @brief 读取已经完成的原始结果；此处没有二次提交或fallback切换。 */
  expected<std::int64_t> await_resume() {
    return std::visit(
        [](auto& operation) -> expected<std::int64_t> {
          if constexpr (std::same_as<std::remove_cvref_t<decltype(operation)>, std::monostate>)
            return std::unexpected{make_error(EINVAL)};
          else
            return operation.await_resume();  // 原生错误、短IO与取消进度保持原值。
        },
        operation_);
  }

 private:
  std::variant<std::monostate, io::native_file_awaiter, io::native_file_close_awaiter, Service>
      operation_;  ///< 只拥有所选分支的状态。
};

/** @brief 返回直接文件awaiter；fallback在原生路径从未被调用。 */
template <class F>
auto execute_file_request(io::io_context context,
                          io::detail::io_request request,
                          F fallback,
                          bool cancellable = true) {
  return file_request_awaiter<F>{
      std::move(context), std::move(request), std::move(fallback), cancellable};
}

/** @brief 将文件原生有符号结果转换为字节数；负值必须已经由协程桥转换成Error。
 */
inline expected<std::size_t> file_byte_count(expected<std::int64_t> result) {
  if (!result)
    return std::unexpected{result.error()};
  if (*result < 0)
    return std::unexpected{make_error(EIO)};  // 拒绝违反桥接契约的负成功值，避免无符号回绕。
  return static_cast<std::size_t>(*result);
}
}  // namespace faio::fs::detail
