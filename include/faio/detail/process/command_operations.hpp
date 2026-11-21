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
auto failed_task(Error error) -> task<expected<T>> {
  co_return std::unexpected{std::move(error)};
}

/** @brief 启动后关闭不需要的管道，再只等待退出状态。 */
inline auto command_status(CommandSnapshot snapshot, io::OperationOptions options)
    -> task<expected<ExitStatus>> {
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
inline auto command_output(CommandSnapshot snapshot, CaptureOptions options)
    -> task<expected<Output>> {
  auto child = co_await spawn_async_impl(std::move(snapshot), options.operation);
  if (!child)
    co_return std::unexpected{child.error()};
  co_return co_await std::move(*child).wait_with_output(options);
}
}  // namespace faio::process::detail

namespace faio::process {
/** @brief 同步完成原生创建与服务接管，返回拥有标准流的 Child。 */
inline auto Command::spawn(io::io_context context) const -> expected<Child> {
  auto snapshot = detail::snapshot_command(*this, std::move(context));
  if (!snapshot)
    return std::unexpected{snapshot.error()};
  return detail::spawn_sync(std::move(*snapshot));
}

/** @brief 立即固定启动快照，等待时交给有界启动服务执行。 */
inline auto Command::spawn_async(io::OperationOptions options, io::io_context context) const
    -> task<expected<Child>> {
  auto snapshot = detail::snapshot_command(*this, std::move(context));
  if (!snapshot)
    return detail::failed_task<Child>(snapshot.error());
  return detail::spawn_async_impl(std::move(*snapshot), options);
}

/** @brief 立即固定配置，异步启动并等待退出状态。 */
inline auto Command::status(io::OperationOptions options, io::io_context context) const
    -> task<expected<ExitStatus>> {
  auto snapshot = detail::snapshot_command(*this, std::move(context));
  if (!snapshot)
    return detail::failed_task<ExitStatus>(snapshot.error());
  return detail::command_status(std::move(*snapshot), options);
}

/** @brief 立即固定配置，异步启动并有界并发捕获 stdout 与 stderr。 */
inline auto Command::output(CaptureOptions options, io::io_context context) const
    -> task<expected<Output>> {
  if (auto valid = detail::validate_capture(options); !valid)
    return detail::failed_task<Output>(valid.error());
  try {
    auto command = *this;
    command.set_stdout(Stdio::piped()).set_stderr(Stdio::piped());
    auto snapshot = detail::snapshot_command(command, std::move(context));
    if (!snapshot)
      return detail::failed_task<Output>(snapshot.error());
    return detail::command_output(std::move(*snapshot), options);
  } catch (const std::bad_alloc&) {
    return detail::failed_task<Output>(make_error(ENOMEM));
  }
}
}  // namespace faio::process
