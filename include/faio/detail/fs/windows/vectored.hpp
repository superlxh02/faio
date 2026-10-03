#pragma once
#include "faio/detail/fs/windows/file.hpp"

namespace faio::fs::detail {
/** @brief 复制描述符数组，payload 仍遵守借用生命周期；限制规模与总字节数。 */
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

/** @brief Windows 任意对齐 vectored 文件 IO；逐段原生 OVERLAPPED，无拼接复制。
 * @details ReadFileScatter/WriteFileGather 要求页对齐且无缓存，不适合通用 iovec。
 *          组合持有同一 File lease 和普通游标锁，短 IO 停止并报告真实进度。
 *          不承诺多个 File 的 vectored 追加作为单一原子事务。
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
        co_return std::unexpected{Error{EOVERFLOW, total}};
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
        co_return std::unexpected{
            Error{count.error().value(), total + count.error().progress(), count.error().domain()}};
      total += *count;
      if (*count < vector.iov_len)
        break;  // EOF/短 IO 不伪装完成后续段。
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
