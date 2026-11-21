/**
 * @file faio/detail/fs/platform/unix/vectored.hpp
 * @brief 分散聚集文件 I/O。
 */
#pragma once
#include "faio/detail/fs/file.hpp"

namespace faio::fs::detail {
/**  @brief 校验分散读取或聚集写入的描述数组；请求复制数组，数据缓冲区仍从调用方借用。 */
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

/**  @brief 持有一份 iovec 元数据；原生提交时移交数组，服务路径则直接读取同一个数组。 */
struct vectored_file_operation {
  std::shared_ptr<file_state>
      state;                  /**< 共享持有操作状态，保证其在异步执行和领取结果期间地址不变。 */
  std::vector<iovec> vectors; /**< 分散聚集 IO 的缓冲区描述，负载仍须由调用方保持有效。 */
  std::uint64_t position;     /**< 当前逻辑游标的字节偏移；更新受所属游标锁保护。 */
  bool write;                 /**< 本操作是否写入，用于关闭与同步时区分活跃读写。 */

  /** @brief 返回当前文件请求的操作类型。 */
  io::detail::operation_kind kind() const noexcept {
    return write ? io::detail::operation_kind::writev : io::detail::operation_kind::readv;
  }

  /** @brief 将紧凑请求展开为完整请求，固定地址、移交资源租约并初始化低频字段。 */
  io::detail::io_request into_request() && {
    io::detail::io_request request;
    request.kind = kind();
    request.fd = state->descriptor.load();
    request.offset = write && state->append ? UINT64_MAX : position;
    request.vectors = std::move(vectors);
    return request;
  }

  /** @brief 执行当前文件请求的同步回退调用。 */
  expected<std::size_t> operator()() const noexcept {
    ssize_t count;
    do {
      if (write && state->append)
        count = ::writev(state->descriptor, vectors.data(), static_cast<int>(vectors.size()));
      else if (write)
        count = ::pwritev(state->descriptor,
                          vectors.data(),
                          static_cast<int>(vectors.size()),
                          static_cast<off_t>(position));
      else
        count = ::preadv(state->descriptor,
                         vectors.data(),
                         static_cast<int>(vectors.size()),
                         static_cast<off_t>(position));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
      return std::unexpected{make_error(errno)};
    return static_cast<std::size_t>(count);
  }
};

/**  @brief 每次分散聚集系统调用使用一个文件租约，普通模式下全程占用共享游标。 */
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
  if (offset)
    co_return file_byte_count(co_await execute_file_request(
        state->context, vectored_file_operation{state, std::move(*vectors), *offset, write}));
  try {
    auto guard = co_await state->cursor->lane.scoped_lock();
    auto count = file_byte_count(co_await execute_file_request(
        state->context,
        vectored_file_operation{state, std::move(*vectors), state->cursor->position, write}));
    state->cursor->position += count ? *count : count.error().progress();
    co_return count;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}
}  // namespace faio::fs::detail
