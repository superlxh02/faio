/**
 * @file faio/detail/io/stream/algorithms.hpp
 * @brief 完整读写与数据复制算法。
 */
#pragma once

#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/coroutine/launch/observed_task.hpp"
#include "faio/detail/coroutine/composition/join_handle.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/platform/error.hpp"
#include "faio/detail/io/traits/async_read.hpp"
#include "faio/detail/io/traits/async_write.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace faio::io {

/**  @brief 读满借用缓冲区；短读继续，EOF/错误携带已完成进度。 */
template <borrowed_async_reader Reader>
task<expected<std::size_t>> read_exact(Reader& reader, std::span<char> buffer) {
  std::size_t total = 0;
  while (total != buffer.size()) {
    auto result = co_await reader.read(buffer.subspan(total));
    if (!result)
      co_return std::unexpected{
          detail::transfer_error(result.error(), total + result.error().progress())};
    if (!*result)
      co_return std::unexpected{Error{Error::UnexpectedEOF, total}};
    if (*result > buffer.size() - total)
      co_return std::unexpected{Error{EIO, total}};
    total += *result;
    co_await this_coro::yield_if_needed();
  }
  co_return total;
}

/**  @brief 写出全部字节；非空输入的零进度写入返回 WriteZero。 */
template <borrowed_async_writer Writer>
task<expected<void>> write_all(Writer& writer, std::span<const char> buffer) {
  std::size_t total = 0;
  while (total != buffer.size()) {
    auto result = co_await writer.write(buffer.subspan(total));
    if (!result)
      co_return std::unexpected{
          detail::transfer_error(result.error(), total + result.error().progress())};
    if (!*result)
      co_return std::unexpected{Error{Error::WriteZero, total}};
    if (*result > buffer.size() - total)
      co_return std::unexpected{Error{EIO, total}};
    total += *result;
    co_await this_coro::yield_if_needed();
  }
  co_return expected<void>{};
}

/** @brief 循环写入直到全部负载完成或失败，组合期间保持方向预留。 */
template <borrowed_async_writer Writer>
task<expected<void>> write_all(Writer& writer, std::string_view buffer) {
  co_return co_await write_all(writer, std::span<const char>{buffer.data(), buffer.size()});
}

/**  @brief 持有缓冲区所有权的组合写入；无论是否发生部分写入，正常结束时都返还原缓冲区。 */
template <borrowed_async_writer Writer>
task<expected<io_transfer>> write_all_buf(Writer& writer, io_buffer buffer) {
  auto result = co_await write_all(writer, buffer.bytes());
  if (!result)
    co_return std::unexpected{result.error()};
  const auto size = buffer.size();
  co_return io_transfer{std::move(buffer), size};
}

/**  @brief 追加读取到 EOF；max_bytes 限制本次追加量，避免无限流无限分配。 */
template <borrowed_async_reader Reader>
task<expected<std::size_t>> read_to_end(
    Reader& reader,
    std::vector<char>& output,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
  std::array<char, 16384> buffer;
  std::size_t total = 0;
  while (total < max_bytes) {
    const auto remaining = std::min(buffer.size(), max_bytes - total);
    auto result = co_await reader.read(std::span<char>{buffer}.first(remaining));
    if (!result)
      co_return std::unexpected{
          detail::transfer_error(result.error(), total + result.error().progress())};
    if (!*result)
      co_return total;
    if (*result > remaining)
      co_return std::unexpected{Error{EIO, total}};
    output.insert(output.end(), buffer.data(), buffer.data() + *result);
    total += *result;
    co_await this_coro::yield_if_needed();
  }
  co_return total;
}

/** @brief 读取字节并追加到字符串，按已完成的字节进度传播错误。 */
template <borrowed_async_reader Reader>
task<expected<std::size_t>> read_to_string(
    Reader& reader,
    std::string& output,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
  std::array<char, 16384> buffer;
  std::size_t total = 0;
  while (total < max_bytes) {
    const auto remaining = std::min(buffer.size(), max_bytes - total);
    auto result = co_await reader.read(std::span<char>{buffer}.first(remaining));
    if (!result)
      co_return std::unexpected{
          detail::transfer_error(result.error(), total + result.error().progress())};
    if (!*result)
      co_return total;
    if (*result > remaining)
      co_return std::unexpected{Error{EIO, total}};
    output.append(buffer.data(), *result);
    total += *result;
    co_await this_coro::yield_if_needed();
  }
  co_return total;
}

/**  @brief 使用固定容量缓冲区复制到 EOF，不随输入长度扩大缓冲区。 */
template <borrowed_async_reader Reader, borrowed_async_writer Writer>
task<expected<std::uint64_t>> copy(Reader& reader,
                                   Writer& writer,
                                   std::size_t buffer_size = 65536) {
  if (!buffer_size)
    co_return std::unexpected{make_error(EINVAL)};
  io_buffer buffer(buffer_size);
  std::uint64_t total = 0;
  for (;;) {
    auto read = co_await reader.read(buffer.writable_bytes());
    if (!read)
      co_return std::unexpected{
          detail::transfer_error(read.error(), static_cast<std::size_t>(total))};
    if (!*read)
      co_return total;
    if (*read > buffer_size)
      co_return std::unexpected{Error{EIO, static_cast<std::size_t>(total)}};
    auto write = co_await write_all(writer, std::span<const char>{buffer.data(), *read});
    if (!write)
      co_return std::unexpected{detail::transfer_error(
          write.error(), static_cast<std::size_t>(total) + write.error().progress())};
    total += *read;
    co_await this_coro::yield_if_needed();
  }
}

namespace detail {
/** @brief 复制一个方向的数据。 */
template <class Reader, class Writer>
task<expected<std::uint64_t>> copy_direction(Reader& reader,
                                             Writer& writer,
                                             std::stop_source& stop,
                                             std::size_t capacity) {
  auto result = co_await copy(reader, writer, capacity);
  if (result) {
    if constexpr (requires { writer.shutdown_write(); }) {
      auto closed = co_await writer.shutdown_write();
      if (!closed)
        result = std::unexpected{transfer_error(closed.error(), static_cast<std::size_t>(*result))};
    } else if constexpr (requires { writer.shutdown(); }) {
      auto closed = co_await writer.shutdown();
      if (!closed)
        result = std::unexpected{transfer_error(closed.error(), static_cast<std::size_t>(*result))};
    }
  }
  if (!result)
    stop.request_stop();
  co_return result;
}
}  // namespace detail

/**  @brief 全双工复制；任一方向错误会取消另一方向，等待两个子任务全部退出后返回。 */
template <class A, class B>
  requires borrowed_async_reader<A> && borrowed_async_writer<A> && borrowed_async_reader<B>
           && borrowed_async_writer<B>
task<expected<std::pair<std::uint64_t, std::uint64_t>>> copy_bidirectional(
    A& first, B& second, std::size_t buffer_size = 65536) {
  std::stop_source stop;
  auto token = co_await this_coro::stop_token();
  std::optional<std::stop_callback<::faio::detail::forward_stop>> parent;
  if (token.stop_possible())
    parent.emplace(token, ::faio::detail::forward_stop{&stop});
  auto scheduler = co_await this_coro::scheduler();
  auto* tracker = ::faio::detail::current_thread_context().task_ctx.scope;
  auto left = join_handle<expected<std::uint64_t>>{::faio::detail::schedule_observed_task(
      scheduler,
      detail::copy_direction(first, second, stop, buffer_size),
      tracker,
      stop.get_token())};
  std::optional<join_handle<expected<std::uint64_t>>> right;
  std::exception_ptr start_error;
  try {
    right.emplace(::faio::detail::schedule_observed_task(
        scheduler,
        detail::copy_direction(second, first, stop, buffer_size),
        tracker,
        stop.get_token()));
  } catch (...) {
    start_error = std::current_exception();
    stop.request_stop();
  }
  auto l = co_await left;
  if (start_error)
    std::rethrow_exception(start_error);
  auto r = co_await *right;
  if (!l && l.error().value() != ECANCELED)
    co_return std::unexpected{l.error()};
  if (!r)
    co_return std::unexpected{r.error()};
  if (!l)
    co_return std::unexpected{l.error()};
  co_return std::pair{*l, *r};
}
}  // namespace faio::io
