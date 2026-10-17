/**
 * @file faio/detail/fs/platform/unix/vectored.hpp
 * @brief 文件与目录异步接口：分散聚集文件 IO。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
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

/** @brief 一份拥有型 iovec 元数据；原生提交移交数组，服务路径直接读取该数组。 */
struct vectored_file_operation {
  std::shared_ptr<file_state> state;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
  std::vector<iovec> vectors;  ///< 分散聚集 IO 的缓冲区描述，负载仍须由调用方保持有效。
  std::uint64_t position;  ///< 当前逻辑游标的字节偏移；更新受所属游标锁保护。
  bool write;  ///< 本操作是否写入，用于关闭与同步时区分活跃读写。

  io::detail::operation_kind kind() const noexcept {
    return write ? io::detail::operation_kind::writev : io::detail::operation_kind::readv;
  }

  /// @brief 把紧凑请求物化为完整稳定请求，移动资源租约并初始化冷字段。
  io::detail::io_request into_request() && {
    io::detail::io_request request;
    request.kind = kind();
    request.fd = state->descriptor.load();
    request.offset = write && state->append ? UINT64_MAX : position;
    /// @brief 稳定请求接管唯一数组，不再复制描述符。
    request.vectors = std::move(vectors);
    return request;
  }

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
}
