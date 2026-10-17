/**
 * @file faio/detail/fs/provider/file_request.hpp
 * @brief 文件与目录异步接口：文件请求路由。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once

#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/execution/execute.hpp"
#include "faio/detail/io/awaiter/native_file.hpp"
#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
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
    /// @brief 只在文件/清理服务线程执行可能阻塞的syscall。
    auto result = std::invoke(fallback);
    /// @brief 保留errno及取消时的真实部分进度。
    if (!result)
      return std::unexpected{result.error()};
    /// @brief 字节数或已发生副作用的条数不能丢失。
    if constexpr (std::is_void_v<typename decltype(result)::value_type>)
      /// @brief 成功的无返回值操作使用与原生CQE一致的零值。
      return std::int64_t{0};
    else
      return static_cast<std::int64_t>(*result);
  }
};

/** @brief 提交前选择一次的文件awaitable；原生分支直接使用IO协程桥。
 * @details 该对象没有协程帧，不创建原生请求之外的job或线程。variant只保存
 * 所选awaitable，结果仍由原始SQE/CQE完成；fallback只在内核无对应opcode时构造。
 */
template <class F>
class file_request_awaiter {
  using Service = execution::execute_awaiter<file_syscall<F>>;  ///< 承载该文件系统调用的类型化阻塞服务等待器。

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
                       bool cancellable)
      : file_request_awaiter(
            std::move(context),
            request.kind,
            [&request](F&) { return std::move(request); },
            std::move(fallback),
            cancellable) {}

  /** @brief 路由以后才构造原生请求；两个分支共用一份拥有型参数。
   * @details 工厂只在原生分支调用，可以把路径/向量移入稳定请求；fallback
   *          直接拥有原参数，不为未选择的原生分支复制描述符或字符串。
   */
  template <class MakeRequest>
  file_request_awaiter(io::io_context context,
                       io::detail::operation_kind kind,
                       MakeRequest make_request,
                       F fallback,
                       bool cancellable) {
    if (io::supports_native(context, kind)) {
      auto request = std::invoke(make_request, fallback);
      /// @brief 查实际内核能力，不按平台名字猜测。
      /// @brief 原始IO桥直接prep/submit及接收CQE。
      if (!cancellable && request.kind == io::detail::operation_kind::close)
        /// @brief 关闭接管后不可被取消提前返回。
        operation_.template emplace<2>(std::move(context),
                                       std::move(request));
      else
        operation_.template emplace<1>(std::move(context),
                                       std::move(request),
                                       cancellable);
    } else if (!cancellable) {
      /// @brief 关闭的fallback使用保留通道，普通队列已满也不能遗失关闭责任。
      /// @brief 保留清理容量，与普通文件服务分开。
      auto service = context.cleanup();
      operation_.template emplace<3>(
          std::move(context), file_syscall<F>{std::move(fallback)}, std::move(service), false);
    } else {
      /// @brief readiness文件IO或缺opcode操作才启动服务。
      auto service = context.blocking();
      operation_.template emplace<3>(
          std::move(context), file_syscall<F>{std::move(fallback)}, std::move(service), true);
    }
  }

  file_request_awaiter(const file_request_awaiter&) = delete;

  file_request_awaiter(file_request_awaiter&&) = default;

  /// @brief 构造和能力选择不执行文件IO。
  bool await_ready() const noexcept { return false; }

  /** @brief 直接委托所选awaiter；不创建转发task或额外协程帧。 */
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    return std::visit(
        [continuation](auto& operation) -> bool {
          /// @brief 原始gate决定即时完成或唯一挂起。
          if constexpr (std::same_as<std::remove_cvref_t<decltype(operation)>, std::monostate>)
            /// @brief 防御空variant；await_resume将其转换为明确的EINVAL。
            return false;
          else
            return operation.await_suspend(continuation);
        },
        operation_);
  }

  /** @brief 读取已经完成的原始结果；此处没有二次提交或fallback切换。 */
  expected<std::int64_t> await_resume() {
    return std::visit(
        [](auto& operation) -> expected<std::int64_t> {
          /// @brief 原生错误、短IO与取消进度保持原值。
          if constexpr (std::same_as<std::remove_cvref_t<decltype(operation)>, std::monostate>)
            return std::unexpected{make_error(EINVAL)};
          else
            return operation.await_resume();
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

/** @brief 参数对象同时描述原生请求和 fallback，路由前不创建第二份参数。
 * @details kind()/into_request() 只负责参数；operator() 只在文件服务执行 syscall。
 */
template <class Operation>
  requires requires(Operation& operation) {
    { operation.kind() } -> std::same_as<io::detail::operation_kind>;
    { std::move(operation).into_request() } -> std::same_as<io::detail::io_request>;
  }
auto execute_file_request(io::io_context context, Operation operation, bool cancellable = true) {
  const auto kind = operation.kind();
  return file_request_awaiter<Operation>{
      std::move(context),
      kind,
      [](Operation& parameters) { return std::move(parameters).into_request(); },
      std::move(operation),
      cancellable};
}

/** @brief 将文件原生有符号结果转换为字节数；负值必须已经由协程桥转换成Error。
 */
inline expected<std::size_t> file_byte_count(expected<std::int64_t> result) {
  if (!result)
    return std::unexpected{result.error()};
  /// @brief 拒绝违反桥接契约的负成功值，避免无符号回绕。
  if (*result < 0)
    return std::unexpected{make_error(EIO)};
  return static_cast<std::size_t>(*result);
}

/** @brief 单块文件 IO 的直接结果适配，不为整数转换创建转发协程帧。
 * @details 参数错误和空缓冲区立即完成；只有有效的非空请求才构造 provider。
 */
template <class Operation>
class file_byte_awaiter {
 public:
  explicit file_byte_awaiter(expected<std::size_t> result) : immediate_(std::move(result)) {}

  file_byte_awaiter(io::io_context context, Operation operation) {
    const auto kind = operation.kind();
    operation_.emplace(
        std::move(context),
        kind,
        [](Operation& parameters) { return std::move(parameters).into_request(); },
        std::move(operation),
        true);
  }

  /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
  bool await_ready() const noexcept { return !operation_; }

  /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    return operation_->await_suspend(continuation);
  }

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  expected<std::size_t> await_resume() {
    return operation_ ? file_byte_count(operation_->await_resume()) : std::move(immediate_);
  }

 private:
  std::optional<file_request_awaiter<Operation>> operation_;  ///< 地址稳定的底层异步操作，当前包装负责其启动、等待与结果交付。
  expected<std::size_t> immediate_{std::size_t{0}};  ///< 无需挂起时保存的即时结果，也用于空读写快速返回。
};

/** @brief 将原生零结果转换为void；该协程就是路径API的唯一操作帧。 */
template <class Operation>
task<expected<void>> void_file_request(io::io_context context, Operation operation) {
  auto result = co_await execute_file_request(std::move(context), std::move(operation));
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}
}
