/**
 * @file faio/detail/fs/platform/windows/vectored.hpp
 * @brief 分散聚集文件 I/O。
 */
#pragma once
#include "faio/detail/fs/platform/windows/file.hpp"

namespace faio::fs::detail {
/**  @brief 复制描述符数组，并限制数组规模和总字节数；数据缓冲区仍须满足借用的生命周期要求。 */
inline expected<std::vector<iovec>> copy_vectors(std::span<const iovec> input) {
  if (input.size() > io::detail::native_iov_limit)
    return std::unexpected{make_error(EINVAL)};
  std::size_t length = 0;
  for (const auto& vector : input) {
    if (vector.iov_len && !vector.iov_base)
      return std::unexpected{make_error(EFAULT)};
    if (vector.iov_len > static_cast<std::size_t>(INT64_MAX) - length)
      return std::unexpected{make_error(EINVAL)};
    length += vector.iov_len;
  }
  return std::vector<iovec>{input.begin(), input.end()};
}

/**
 *  @brief 支持任意对齐的 Windows 分散聚集文件 I/O，逐段提交原生 OVERLAPPED 请求，无需拼接复制。
 * @details ReadFileScatter/WriteFileGather 要求页对齐且无缓存，不适合通用 iovec。
 *          组合操作持有同一个 File 租约和普通游标锁，遇到短 I/O 时停止并报告实际进度。
 *          不保证多个 File 的分散聚集追加操作构成一个原子事务。
 */
inline task<expected<std::size_t>> vectored_file(std::shared_ptr<file_state> state,
                                                 expected<std::vector<iovec>> vectors,
                                                 std::optional<std::uint64_t> offset,
                                                 bool write) {
  if (!vectors)
    co_return std::unexpected{vectors.error()};
  auto lease = file_lease::acquire(state, write);
  if (!lease)
    co_return std::unexpected{lease.error()};
  if (offset && !valid_offset(*offset))
    co_return std::unexpected{make_error(EOVERFLOW)};
  if (offset && write && state->append)
    co_return std::unexpected{make_error(EINVAL)};
  auto perform = [state, &vectors, write](std::uint64_t position) -> task<expected<std::size_t>> {
    std::size_t total = 0;
    for (const auto& vector : *vectors) {
      if (!vector.iov_len)
        continue;
      if (total > static_cast<std::uint64_t>(INT64_MAX) - position)
        co_return std::unexpected{::faio::error{EOVERFLOW, total}};
      expected<std::size_t> count;
      if (write)
        count = co_await write_block(state,
                                     {static_cast<const char*>(vector.iov_base), vector.iov_len},
                                     position + total,
                                     state->append);
      else
        count = co_await read_block(
            state, {static_cast<char*>(vector.iov_base), vector.iov_len}, position + total);
      if (!count)
        co_return std::unexpected{count.error().with_progress(total + count.error().progress())};
      total += *count;
      if (*count < vector.iov_len)
        break;
    }
    co_return total;
  };
  if (offset)
    co_return co_await perform(*offset);
  try {
    auto guard = co_await state->cursor->lane.scoped_lock();
    auto count = co_await perform(state->cursor->position);
    state->cursor->position += count ? *count : count.error().progress();
    co_return count;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}
}  // namespace faio::fs::detail
