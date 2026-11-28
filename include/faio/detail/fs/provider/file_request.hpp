/**
 * @file faio/detail/fs/provider/file_request.hpp
 * @brief 文件请求路由。
 */
#pragma once

#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/blocking/call.hpp"
#include "faio/detail/io/awaiter/native_file.hpp"
#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <variant>

namespace faio::fs::detail {
/**
 * @brief 转换文件服务调用的结果，将 void 或整数结果统一为有符号 expected。
 * @tparam F 持有实际文件系统调用参数的可调用对象，仅在回退分支构造。
 */
template <class F>
struct file_syscall {
  F fallback; /**< 仅回退分支持有该函数；原生分支不创建服务任务。 */

  /** @brief 执行当前文件请求的同步回退调用。 */
  expected<std::int64_t> operator()() {
    auto result = std::invoke(fallback);
    if (!result)
      return std::unexpected{result.error()};
    if constexpr (std::is_void_v<typename decltype(result)::value_type>)
      return std::int64_t{0};
    else
      return static_cast<std::int64_t>(*result);
  }
};

/**
 *  @brief 在提交前选择实现分支的文件等待器；原生分支直接使用 I/O 协程桥接。
 * @details 该对象没有协程帧，除原生请求外不创建额外任务或线程。variant 只保存
 * 选中的等待器，结果仍由原始 SQE/CQE 完成；仅在内核没有对应操作码时构造回退分支。
 */
template <class F>
class file_request_awaiter {
  using service_type =
      blocking::call_awaiter<file_syscall<F>>; /**< 承载该文件系统调用的类型化阻塞服务等待器。 */
#if !defined(FAIO_TEST_BACKEND_INJECTION)                                           \
    && ((defined(__linux__) && (!defined(FAIO_HAS_IO_URING) || !FAIO_HAS_IO_URING)) \
        || defined(__APPLE__) || defined(__FreeBSD__))
  /** @brief 按后端能力选择原生文件请求存储。 */
  static constexpr bool native_provider = false;
#else
  static constexpr bool native_provider = true;
#endif
  using native_type = std::conditional_t<native_provider, io::native_file_awaiter, std::monostate>;
  using native_close =
      std::conditional_t<native_provider, io::native_file_close_awaiter, std::monostate>;

 public:
  /**
   *  @brief 仅在提交前根据能力选择一次分支，已接纳请求返回错误时不会重新执行。
   * @param context 文件所属的固定服务租约，不读取恢复线程的 TLS。
   * @param request 独立持有路径或 iovec 描述符的通用请求，fd 由外层活跃租约保护。
   * @param fallback 仅在原生操作码不可用时交给独立服务执行的系统调用。
   * @param cancellable close 等清理操作设为 false；借用数据的业务 I/O 设为 true，并严格等待排空。
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

  /**
   *  @brief 选择分支后才构造原生请求；两个分支共用一份独立持有的参数。
   * @details 工厂只在原生分支调用，可以把路径/向量移入稳定请求；fallback
   *          直接拥有原参数，不为未选择的原生分支复制描述符或字符串。
   */
  template <class make_request_type>
  file_request_awaiter(io::io_context context,
                       io::detail::operation_kind kind,
                       make_request_type make_request,
                       F fallback,
                       bool cancellable) {
    if constexpr (native_provider) {
      if (io::supports_native(context, kind)) {
        auto request = std::invoke(make_request, fallback);
        if (!cancellable && request.kind == io::detail::operation_kind::CLOSE)
          operation_.template emplace<2>(std::move(context), std::move(request));
        else
          operation_.template emplace<1>(std::move(context), std::move(request), cancellable);
        return;
      }
    }
    if (!cancellable) {
      auto service = context.cleanup();
      operation_.template emplace<3>(
          std::move(context), file_syscall<F>{std::move(fallback)}, std::move(service), false);
    } else {
      auto service = context.blocking();
      operation_.template emplace<3>(
          std::move(context), file_syscall<F>{std::move(fallback)}, std::move(service), true);
    }
  }

  /** @brief 禁止复制此对象。 */
  file_request_awaiter(const file_request_awaiter&) = delete;

  /** @brief 转移文件等待器状态和资源租约。 */
  file_request_awaiter(file_request_awaiter&&) = default;

  /** @brief 构造对象和根据能力选择实现时不执行文件 I/O。 */
  bool await_ready() const noexcept { return false; }

  /**  @brief 直接委托所选等待器，不创建转发 task 或额外协程帧。 */
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    return std::visit(
        [continuation](auto& operation) -> bool {
          if constexpr (std::same_as<std::remove_cvref_t<decltype(operation)>, std::monostate>)
            return false;
          else
            return operation.await_suspend(continuation);
        },
        operation_);
  }

  /**  @brief 读取已完成的原始结果，不再次提交请求或切换到回退路径。 */
  expected<std::int64_t> await_resume() {
    return std::visit(
        [](auto& operation) -> expected<std::int64_t> {
          if constexpr (std::same_as<std::remove_cvref_t<decltype(operation)>, std::monostate>)
            return std::unexpected{make_error(EINVAL)};
          else
            return operation.await_resume();
        },
        operation_);
  }

 private:
  std::variant<std::monostate, native_type, native_close, service_type>
      operation_; /**< 只拥有所选分支的状态。 */
};

/**  @brief 返回直接等待文件操作的等待器，原生路径不会调用 fallback。 */
template <class F>
auto execute_file_request(io::io_context context,
                          io::detail::io_request request,
                          F fallback,
                          bool cancellable = true) {
  return file_request_awaiter<F>{
      std::move(context), std::move(request), std::move(fallback), cancellable};
}

/**
 *  @brief 参数对象同时描述原生请求和回退调用，选择分支前无需创建第二份参数。
 * @details kind()/into_request() 只处理参数，operator() 仅在文件服务中执行系统调用。
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

/**  @brief 将文件操作返回的原生有符号结果转换为字节数；负值须先由协程桥接逻辑转换为 Error。 */
inline expected<std::size_t> file_byte_count(expected<std::int64_t> result) {
  if (!result)
    return std::unexpected{result.error()};
  if (*result < 0)
    return std::unexpected{make_error(EIO)};
  return static_cast<std::size_t>(*result);
}

/**
 *  @brief 直接适配单块文件 I/O 的结果，无需为整数转换创建转发协程帧。
 * @details 参数错误和空缓冲区立即完成；只有有效的非空请求才构造 provider。
 */
template <class Operation>
class file_byte_awaiter {
 public:
  /** @brief 包装文件请求并交付字节数结果。 */
  explicit file_byte_awaiter(expected<std::size_t> result) : immediate_(std::move(result)) {}

  /** @brief 包装文件请求并交付字节数结果。 */
  file_byte_awaiter(io::io_context context, Operation operation) {
    const auto kind = operation.kind();
    operation_.emplace(
        std::move(context),
        kind,
        [](Operation& parameters) { return std::move(parameters).into_request(); },
        std::move(operation),
        true);
  }

  /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
  bool await_ready() const noexcept { return !operation_; }

  /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    return operation_->await_suspend(continuation);
  }

  /** @brief 完成交接后领取结果，并按接口约定传播失败或取消。 */
  expected<std::size_t> await_resume() {
    return operation_ ? file_byte_count(operation_->await_resume()) : std::move(immediate_);
  }

 private:
  std::optional<file_request_awaiter<Operation>>
      operation_; /**< 地址固定的底层异步操作，由本等待器负责启动、等待和交付结果。 */
  expected<std::size_t> immediate_{
      std::size_t{0}}; /**< 无需挂起时保存的即时结果，也用于空读写快速返回。 */
};

/**  @brief 将原生零值结果转换为 void；该协程是路径 API 唯一的操作帧。 */
template <class Operation>
task<expected<void>> void_file_request(io::io_context context, Operation operation) {
  auto result = co_await execute_file_request(std::move(context), std::move(operation));
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}
}  // namespace faio::fs::detail
