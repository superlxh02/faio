/**
 * @file faio/detail/process/pipe_operations.hpp
 * @brief 将子进程管道接入既有异步 IO 与取消排空协议。
 */
#pragma once
#include "faio/detail/process/child_stdio.hpp"
#include "faio/detail/process/exit_wait_operation.hpp"
#include "faio/detail/io/stream/operation.hpp"
#include "faio/detail/io/awaiter/read.hpp"
#include "faio/detail/io/awaiter/write.hpp"
#include "faio/detail/io/awaiter/close.hpp"

namespace faio::process::detail {
/** @brief 检测管道最初所属的 IO 域是否已停止。 */
inline bool pipe_runtime_stopped(const std::shared_ptr<pipe_state>& pipe) noexcept {
  return pipe && pipe->resource() && pipe->resource()->owner && pipe->resource()->owner->stopped();
}

/** @brief 先分配地址固定的状态，再将原生管道所有权交给 I/O 域。 */
inline auto pipe_state::adopt(io::io_context context,
                              io::detail::native_descriptor descriptor,
                              bool writer) -> expected<std::shared_ptr<pipe_state>> {
  try {
    if (!context || context.stopped())
      return std::unexpected{process_error(errc::RUNTIME_SHUTDOWN)};

    struct factory final : pipe_state {
      /** @brief 原样转发构造参数，通过 make_shared 在一次分配中创建地址固定的状态。 */
      explicit factory(bool writing) : pipe_state({}, writing) {}
    };

    auto state = std::make_shared<factory>(writer);
#if defined(_WIN32)
    auto resource = context.domain()->adopt_pipe(descriptor);
#else
    (void)io::detail::unix_operations::prepare_borrowed_descriptor(descriptor);
    auto resource = io::detail::adopt_resource(context, descriptor);
#endif
    state->resource_ = std::move(resource);
    return state;
  } catch (const std::bad_alloc&) {
    return std::unexpected{make_error(ENOMEM)};
  } catch (const std::system_error& error) {
    return std::unexpected{make_error(error.code().value())};
  }
}

/** @brief 提交管道读取；管道不能按偏移寻址，截止时间由既有 I/O 请求处理。 */
inline auto raw_pipe_read(std::shared_ptr<pipe_state> pipe,
                          std::span<char> bytes,
                          io::operation_options options) -> task<expected<std::size_t>> {
  auto awaiter =
      io::read(
          pipe->resource(), bytes.data(), bytes.size(), std::numeric_limits<std::uint64_t>::max())
          .empty_success();
  if (options.deadline)
    awaiter.set_timeout_at(*options.deadline);
  co_return co_await awaiter;
}

/** @brief 提交管道写入；管道不能按偏移寻址，截止时间由既有 I/O 请求处理。 */
inline auto raw_pipe_write(std::shared_ptr<pipe_state> pipe,
                           std::span<const char> bytes,
                           io::operation_options options) -> task<expected<std::size_t>> {
  auto awaiter =
      io::write(
          pipe->resource(), bytes.data(), bytes.size(), std::numeric_limits<std::uint64_t>::max())
          .empty_success();
  if (options.deadline)
    awaiter.set_timeout_at(*options.deadline);
  co_return co_await awaiter;
}

/** @brief 验证管道状态，合并父级与显式取消请求，再等待底层读取排空。 */
inline auto pipe_read(std::shared_ptr<pipe_state> pipe,
                      std::span<char> bytes,
                      io::operation_options options) -> task<expected<std::size_t>> {
  if (!pipe)
    co_return std::unexpected{make_error(EBADF)};
  if (pipe_runtime_stopped(pipe))
    co_return std::unexpected{process_error(errc::RUNTIME_SHUTDOWN)};
  if (!pipe->is_open())
    co_return std::unexpected{make_error(EBADF)};
  auto parent = co_await this_coro::stop_token();
  if (auto checked = preflight(options, parent); !checked)
    co_return std::unexpected{checked.error()};
  std::stop_source combined;
  auto forward = [&combined] {
    combined.request_stop();
  };
  std::stop_callback from_parent{parent, forward};
  std::stop_callback from_explicit{options.stop, forward};
  co_return co_await io::detail::bind_stop(raw_pipe_read(pipe, bytes, options),
                                           combined.get_token());
}

/** @brief 验证管道状态，合并父级与显式取消请求，再等待底层写入排空。 */
inline auto pipe_write(std::shared_ptr<pipe_state> pipe,
                       std::span<const char> bytes,
                       io::operation_options options) -> task<expected<std::size_t>> {
  if (!pipe)
    co_return std::unexpected{make_error(EBADF)};
  if (pipe_runtime_stopped(pipe))
    co_return std::unexpected{process_error(errc::RUNTIME_SHUTDOWN)};
  if (!pipe->is_open())
    co_return std::unexpected{make_error(EBADF)};
  auto parent = co_await this_coro::stop_token();
  if (auto checked = preflight(options, parent); !checked)
    co_return std::unexpected{checked.error()};
  std::stop_source combined;
  auto forward = [&combined] {
    combined.request_stop();
  };
  std::stop_callback from_parent{parent, forward};
  std::stop_callback from_explicit{options.stop, forward};
  co_return co_await io::detail::bind_stop(raw_pipe_write(pipe, bytes, options),
                                           combined.get_token());
}

/** @brief 持有 io_buffer 到读取结束，并按实际字节数更新有效长度。 */
inline auto pipe_read_owned(std::shared_ptr<pipe_state> pipe,
                            io::io_buffer buffer,
                            io::operation_options options) -> task<expected<io::io_transfer>> {
  auto result = co_await pipe_read(
      std::move(pipe), std::span<char>{buffer.data(), buffer.capacity()}, options);
  if (!result)
    co_return std::unexpected{result.error()};
  buffer.set_size(*result);
  co_return io::io_transfer{std::move(buffer), *result};
}

/** @brief 只写入 io_buffer 的有效字节，同时保留其所有权。 */
inline auto pipe_write_owned(std::shared_ptr<pipe_state> pipe,
                             io::io_buffer buffer,
                             io::operation_options options) -> task<expected<io::io_transfer>> {
  auto result = co_await pipe_write(
      std::move(pipe), std::span<const char>{buffer.data(), buffer.size()}, options);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return io::io_transfer{std::move(buffer), *result};
}

/** @brief 等待 IO 域关闭并排空已经接纳的管道请求。 */
inline auto raw_pipe_close(io::detail::resource_ptr resource) -> task<expected<void>> {
  co_return co_await io::close(std::move(resource));
}

/** @brief 先请求关闭，再等待 IO 域排空；取消不撤销已接纳的关闭责任。 */
inline auto pipe_state::close(io::operation_options options) -> task<expected<void>> {
  request_close();
  if (!resource_)
    co_return expected<void>{};
  auto body = [resource =
                   resource_](io::detail::stream_operation_state<void>& state) -> task<void> {
    auto result = co_await io::detail::bind_stop(raw_pipe_close(resource), std::stop_token{});
    state.try_commit([&]() noexcept -> expected<void> { return std::move(result); });
  };
  co_return co_await io::detail::run_operation<void>(options, std::move(body));
}

/** @brief 将关闭请求交给地址固定的管道状态处理；包装为空时返回 EBADF。 */
inline auto pipe_close(std::shared_ptr<pipe_state> pipe, io::operation_options options)
    -> task<expected<void>> {
  if (!pipe)
    co_return std::unexpected{make_error(EBADF)};
  co_return co_await pipe->close(options);
}

/** @brief 管道无用户态缓存，仅检查有效性、取消及截止时间。 */
inline auto pipe_flush(std::shared_ptr<pipe_state> pipe, io::operation_options options)
    -> task<expected<void>> {
  if (!pipe)
    co_return std::unexpected{make_error(EBADF)};
  if (pipe_runtime_stopped(pipe))
    co_return std::unexpected{process_error(errc::RUNTIME_SHUTDOWN)};
  if (!pipe->is_open())
    co_return std::unexpected{make_error(EBADF)};
  co_return preflight(options, co_await this_coro::stop_token());
}
}  // namespace faio::process::detail
