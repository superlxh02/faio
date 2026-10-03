#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/windows/vectored.hpp"
#else
#include "faio/detail/fs/file.hpp"

namespace faio::fs::detail {
/** @brief 校验散布/收集数组；数组由请求复制，payload 仍由调用者借用。 */
inline expected<std::vector<iovec>> copy_vectors(std::span<const iovec> input) {
  if (input.size() > IOV_MAX)
    return std::unexpected{make_error(EINVAL)};
  std::size_t total = 0;
  for (const auto& vector : input) {
    if (vector.iov_len && !vector.iov_base)
      return std::unexpected{make_error(EFAULT)};
    if (vector.iov_len > static_cast<std::size_t>(SSIZE_MAX) - total)
      return std::unexpected{make_error(EINVAL)};
    total += vector.iov_len;
  }
  return std::vector<iovec>{input.begin(), input.end()};
}

/** @brief 一个 vectored syscall 使用一份文件租约，普通模式全程占用共享游标。 */
inline task<expected<std::size_t>> vectored_file(std::shared_ptr<file_state> state,
                                                 expected<std::vector<iovec>> vectors,
                                                 std::optional<std::uint64_t> offset,
                                                 bool write) {
  if (!vectors)
    co_return std::unexpected{vectors.error()};
  auto lease = file_lease::acquire(state, write);
  if (!lease)
    co_return std::unexpected{lease.error()};
  if (vectors->empty())
    co_return std::size_t{0};
  if (offset && !valid_offset(*offset))
    co_return std::unexpected{make_error(EOVERFLOW)};
  if (offset && write && state->append)
    co_return std::unexpected{make_error(EINVAL)};
  auto perform = [state, &vectors, write](std::uint64_t position) -> expected<std::size_t> {
    ssize_t count;
    do {
      if (write && state->append)
        count = ::writev(state->descriptor, vectors->data(), static_cast<int>(vectors->size()));
      else if (write)
        count = ::pwritev(state->descriptor,
                          vectors->data(),
                          static_cast<int>(vectors->size()),
                          static_cast<off_t>(position));
      else
        count = ::preadv(state->descriptor,
                         vectors->data(),
                         static_cast<int>(vectors->size()),
                         static_cast<off_t>(position));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
      return std::unexpected{make_error(errno)};
    return static_cast<std::size_t>(count);
  };
  auto native_request = [state, &vectors, write](std::uint64_t position) {
    io::detail::io_request request;
    request.kind = write ? io::detail::operation_kind::writev : io::detail::operation_kind::readv;
    request.fd = state->descriptor.load();  // 整个vectored_file持有File活跃租约。
    request.offset = write && state->append ? UINT64_MAX : position;
    request.vectors = *vectors;  // SQE描述符存入稳定operation，payload仍由外层借用保护。
    return request;
  };
  if (offset)
    co_return file_byte_count(co_await execute_file_request(
        state->context, native_request(*offset), [perform, offset] { return perform(*offset); }));
  try {
    auto guard = co_await state->cursor->lane.scoped_lock();
    auto count = file_byte_count(co_await execute_file_request(
        state->context, native_request(state->cursor->position), [state, perform] {
          return perform(state->cursor->position);
        }));
    state->cursor->position += count ? *count : count.error().progress();
    co_return count;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}
}  // namespace faio::fs::detail

#endif  // _WIN32
