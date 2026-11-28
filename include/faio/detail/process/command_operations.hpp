/**
 * @file faio/detail/process/command_operations.hpp
 * @brief 命令调用时快照与启动、等待、捕获操作的衔接。
 */
#pragma once
#include "faio/detail/process/launch_operation.hpp"
#include "faio/detail/process/capture.hpp"

namespace faio::process::detail {
/** @brief 构造以给定错误完成的异步结果。 */
template <class T>
auto failed_task(error error) -> task<expected<T>> {
  co_return std::unexpected{std::move(error)};
}

/** @brief 启动后关闭不需要的管道，再只等待退出状态。 */
inline auto command_status(command_snapshot snapshot, io::operation_options options)
    -> task<expected<exit_status>> {
  auto child = co_await spawn_async_impl(std::move(snapshot), options);
  if (!child)
    co_return std::unexpected{child.error()};
  if (auto stream = child->take_stdin())
    (void)co_await stream->close();
  if (auto stream = child->take_stdout())
    (void)co_await stream->close();
  if (auto stream = child->take_stderr())
    (void)co_await stream->close();
  co_return co_await child->wait(options);
}

/** @brief 启动后将 Child 交给捕获状态，并发等待进程退出和输出。 */
inline auto command_output(command_snapshot snapshot, capture_options options)
    -> task<expected<::faio::process::output>> {
  auto child = co_await spawn_async_impl(std::move(snapshot), options.operation);
  if (!child)
    co_return std::unexpected{child.error()};
  co_return co_await std::move(*child).wait_with_output(options);
}
}  // namespace faio::process::detail

namespace faio::process {
/** @brief 同步完成原生创建与服务接管，返回拥有标准流的 Child。 */
inline auto command::spawn(io::io_context context) const -> expected<child> {
  auto snapshot = detail::snapshot_command(*this, std::move(context));
  if (!snapshot)
    return std::unexpected{snapshot.error()};
  return detail::spawn_sync(std::move(*snapshot));
}

/** @brief 立即固定启动快照，等待时交给有界启动服务执行。 */
inline auto command::spawn_async(io::operation_options options, io::io_context context) const
    -> task<expected<child>> {
  auto snapshot = detail::snapshot_command(*this, std::move(context));
  if (!snapshot)
    return detail::failed_task<child>(snapshot.error());
  return detail::spawn_async_impl(std::move(*snapshot), options);
}

/** @brief 立即固定配置，异步启动并等待退出状态。 */
inline auto command::status(io::operation_options options, io::io_context context) const
    -> task<expected<exit_status>> {
  auto snapshot = detail::snapshot_command(*this, std::move(context));
  if (!snapshot)
    return detail::failed_task<exit_status>(snapshot.error());
  return detail::command_status(std::move(*snapshot), options);
}

/** @brief 立即固定配置，异步启动并有界并发捕获 stdout 与 stderr。 */
inline auto command::output(capture_options options, io::io_context context) const
    -> task<expected<::faio::process::output>> {
  if (auto valid = detail::validate_capture(options); !valid)
    return detail::failed_task<::faio::process::output>(valid.error());
  try {
    auto command = *this;
    command.set_stdout(stdio::piped()).set_stderr(stdio::piped());
    auto snapshot = detail::snapshot_command(command, std::move(context));
    if (!snapshot)
      return detail::failed_task<::faio::process::output>(snapshot.error());
    return detail::command_output(std::move(*snapshot), options);
  } catch (const std::bad_alloc&) {
    return detail::failed_task<::faio::process::output>(make_error(ENOMEM));
  }
}
}  // namespace faio::process
