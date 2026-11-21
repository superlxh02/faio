/**
 * @file faio/detail/fs/platform/unix/file.hpp
 * @brief 异步文件对象。
 */
#pragma once

#include "faio/detail/blocking/call.hpp"
#include "faio/detail/fs/core/file_lease.hpp"
#include "faio/detail/fs/core/seek_from.hpp"
#if defined(__linux__)
#include "faio/detail/fs/platform/linux/native_metadata.hpp"
#endif
#include "faio/detail/fs/open_options.hpp"
#include "faio/detail/fs/platform/unix/sync.hpp"
#include "faio/detail/fs/provider/file_request.hpp"
#include "faio/detail/io/base/buffer.hpp"
#include "faio/detail/sync/mutex.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <limits>
#include <memory>
#include <mutex>
#include <sys/uio.h>
#include <unistd.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace faio::fs {
namespace detail {
/** @brief 检查字节偏移是否位于平台可表示范围。 */
inline bool valid_offset(std::uint64_t offset) noexcept {
  return offset <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
}

/** @brief 在给定字节偏移读取，不修改逻辑游标。 */
inline expected<std::size_t> positional_read(int fd,
                                             std::span<char> buffer,
                                             std::uint64_t offset) noexcept {
  if (!valid_offset(offset))
    return std::unexpected{make_error(EOVERFLOW)};
  if (buffer.empty())
    return 0;
  ssize_t result;
  do {
    result = ::pread(fd,
                     buffer.data(),
                     std::min(buffer.size(), static_cast<std::size_t>(SSIZE_MAX)),
                     static_cast<off_t>(offset));
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return std::unexpected{make_error(errno)};
  return static_cast<std::size_t>(result);
}

/** @brief 在给定字节偏移写入，不修改逻辑游标。 */
inline expected<std::size_t> positional_write(int fd,
                                              std::span<const char> buffer,
                                              std::uint64_t offset,
                                              bool append) noexcept {
  if (!valid_offset(offset))
    return std::unexpected{make_error(EOVERFLOW)};
  if (buffer.empty())
    return 0;
  ssize_t result;
  do {
    result = append ? ::write(fd,
                              buffer.data(),
                              std::min(buffer.size(), static_cast<std::size_t>(SSIZE_MAX)))
                    : ::pwrite(fd,
                               buffer.data(),
                               std::min(buffer.size(), static_cast<std::size_t>(SSIZE_MAX)),
                               static_cast<off_t>(offset));
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return std::unexpected{make_error(errno)};
  return static_cast<std::size_t>(result);
}

/**  @brief 原生请求与服务调用共用一份单块读写参数，只构造选中的分支。 */
template <bool Writing>
struct file_block_operation {
  std::shared_ptr<file_state>
      state; /**< 共享持有操作状态，保证其在异步执行和领取结果期间地址不变。 */
  std::span<std::conditional_t<Writing, const char, char>>
      buffer;           /**< 本次操作借用的数据区，操作安全完成前调用方须保持有效。 */
  std::uint64_t offset; /**< 本次文件操作的字节偏移，定位基准由对应接口约定。 */
  bool append{};        /**< 是否采用追加写入语义，区别于指定偏移写入。 */

  /** @brief 返回当前文件请求的操作类型。 */
  static constexpr io::detail::operation_kind kind() noexcept {
    return Writing ? io::detail::operation_kind::write : io::detail::operation_kind::read;
  }

  /** @brief 将紧凑请求展开为完整请求，固定地址、移交资源租约并初始化低频字段。 */
  io::detail::io_request into_request() && {
    io::detail::io_request request;
    request.kind = kind();
    request.fd = state->descriptor.load();
    if constexpr (Writing)
      request.const_buffer = buffer.data();
    else
      request.buffer = buffer.data();
    request.length = std::min(buffer.size(), static_cast<std::size_t>(INT_MAX));
    request.offset = append ? UINT64_MAX : offset;
    return request;
  }

  /** @brief 执行当前文件请求的同步回退调用。 */
  expected<std::size_t> operator()() const noexcept {
    if constexpr (Writing)
      return positional_write(state->descriptor, buffer, offset, append);
    else
      return positional_read(state->descriptor, buffer, offset);
  }
};

/**  @brief 单块操作的直接等待器，保留参数校验，结果转换无需分配子协程帧。 */
template <bool Writing>
inline auto transfer_block(std::shared_ptr<file_state> state,
                           std::span<std::conditional_t<Writing, const char, char>> buffer,
                           std::uint64_t offset,
                           bool append = false) {
  using Result = file_byte_awaiter<file_block_operation<Writing>>;
  if (!valid_offset(offset))
    return Result{std::unexpected{make_error(EOVERFLOW)}};
  if (buffer.empty())
    return Result{std::size_t{0}};
  auto context = state->context;
  return Result{std::move(context),
                file_block_operation<Writing>{std::move(state), buffer, offset, append}};
}

/** @brief 通过文件请求读取一个有界数据块。 */
inline auto read_block(std::shared_ptr<file_state> state,
                       std::span<char> buffer,
                       std::uint64_t offset) {
  return transfer_block<false>(std::move(state), buffer, offset);
}

/**  @brief append 使用流式 offset，保留内核通过 O_APPEND 提供的原子追加语义。 */
inline auto write_block(std::shared_ptr<file_state> state,
                        std::span<const char> buffer,
                        std::uint64_t offset,
                        bool append) {
  return transfer_block<true>(std::move(state), buffer, offset, append);
}

/** @brief 读取文件字节，按需串行推进逻辑游标。 */
inline task<expected<std::size_t>> read_file(std::shared_ptr<file_state> state,
                                             std::span<char> buffer,
                                             std::optional<std::uint64_t> offset) {
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
  if (offset)
    co_return co_await read_block(state, buffer, *offset);
  try {
    auto guard = co_await state->cursor->lane.scoped_lock();
    auto result = co_await read_block(state, buffer, state->cursor->position);
    if (result)
      state->cursor->position += *result;
    else
      state->cursor->position += result.error().progress();
    co_return result;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}

/** @brief 写入文件字节，按需串行推进逻辑游标。 */
inline task<expected<std::size_t>> write_file(std::shared_ptr<file_state> state,
                                              std::span<const char> buffer,
                                              std::optional<std::uint64_t> offset) {
  auto lease = file_lease::acquire(state, true);
  if (!lease)
    co_return std::unexpected{lease.error()};
  if (offset && state->append)
    co_return std::unexpected{make_error(EINVAL)};
  if (offset)
    co_return co_await write_block(state, buffer, *offset, false);
  try {
    auto guard = co_await state->cursor->lane.scoped_lock();
    auto result = co_await write_block(state, buffer, state->cursor->position, state->append);
    if (result)
      state->cursor->position += *result;
    else
      state->cursor->position += result.error().progress();
    co_return result;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}

/**
 * @brief 组合读写全程持有游标租约，将数据按每块至多 2 MiB 单独提交到文件 provider。
 * @details 成员函数返回时已复制 state，惰性 task 不借用 this，因此移动或析构 File 不会使其失效。
 * 共享同一游标的 clone、seek 和普通读写须等待整个组合操作结束；取消错误仍保留已完成的字节数。
 */
template <bool Writing>
inline task<expected<std::conditional_t<Writing, void, std::size_t>>> transfer_exact(
    std::shared_ptr<file_state> state,
    std::span<std::conditional_t<Writing, const char, char>> buffer) {
  auto lease = file_lease::acquire(state, Writing);
  if (!lease)
    co_return std::unexpected{lease.error()};
  std::size_t total = 0;
  try {
    auto guard = co_await state->cursor->lane.scoped_lock();
    while (total < buffer.size()) {
      constexpr std::size_t transfer_chunk_size = 2 * 1024 * 1024;
      auto block = buffer.subspan(total, std::min(transfer_chunk_size, buffer.size() - total));
      expected<std::size_t> result;
      if constexpr (Writing)
        result = co_await write_block(state, block, state->cursor->position, state->append);
      else
        result = co_await read_block(state, block, state->cursor->position);
      const auto progress = result ? *result : result.error().progress();
      state->cursor->position += progress;
      if (!result)
        co_return std::unexpected{result.error().with_progress(total + progress)};
      if (!*result)
        co_return std::unexpected{Error{Writing ? Error::WriteZero : Error::UnexpectedEOF, total}};
      total += *result;
      co_await this_coro::yield_if_needed();
    }
    if constexpr (Writing)
      co_return expected<void>{};
    else
      co_return total;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{Error{ECANCELED, total}};
  }
}

/** @brief 循环完成文件写入，失败时保存传输进度。 */
inline task<expected<void>> write_all_file(std::shared_ptr<file_state> state,
                                           std::span<const char> buffer) {
  return transfer_exact<true>(std::move(state), buffer);
}

/** @brief 持有文件缓冲区的所有权，直到读取完成。 */
inline task<expected<io::io_transfer>> read_owned(std::shared_ptr<file_state> state,
                                                  io::io_buffer buffer,
                                                  std::optional<std::uint64_t> offset) {
  auto result = co_await read_file(std::move(state), buffer.writable_bytes(), offset);
  if (!result)
    co_return std::unexpected{result.error()};
  buffer.set_size(*result);
  co_return io::io_transfer{std::move(buffer), *result};
}

/** @brief 接管数据缓冲区，写入完成后返还其所有权和写入进度。 */
inline task<expected<io::io_transfer>> write_owned(std::shared_ptr<file_state> state,
                                                   io::io_buffer buffer,
                                                   std::optional<std::uint64_t> offset) {
  auto result = co_await write_file(std::move(state), buffer.bytes(), offset);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return io::io_transfer{std::move(buffer), *result};
}

/** @brief 构造原生或服务回退的文件操作请求。 */
template <class F>
auto file_request(std::shared_ptr<file_state> state, F function) -> task<
    expected<typename blocking::detail::unwrap_expected<std::invoke_result_t<F&, int>>::type>> {
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
  co_return co_await blocking::call(state->context,
                                    [state, function = std::move(function)]() mutable {
                                      return std::invoke(function, state->descriptor);
                                    });
}

/** @brief 验证定位结果并串行更新文件逻辑游标。 */
inline task<expected<std::uint64_t>> seek_file(std::shared_ptr<file_state> state, seek_from from) {
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
  try {
    auto guard = co_await state->cursor->lane.scoped_lock();
    std::uint64_t base = 0;
    if (from.base == seek_from::origin::current)
      base = state->cursor->position;
    else if (from.base == seek_from::origin::end) {
#if defined(__linux__)
      if (io::supports_native(state->context, io::detail::operation_kind::statx)) {
        auto attributes = co_await native_metadata_awaiter{
            state->context, state->descriptor.load(), "", AT_EMPTY_PATH};
        if (!attributes)
          co_return std::unexpected{attributes.error()};
        base = attributes->len();
      } else
#endif
      {
        auto metadata =
            co_await blocking::call(state->context, [state]() -> expected<std::uint64_t> {
              struct stat attributes;
              if (::fstat(state->descriptor, &attributes))
                return std::unexpected{make_error(errno)};
              return static_cast<std::uint64_t>(attributes.st_size);
            });
        if (!metadata)
          co_return std::unexpected{metadata.error()};
        base = *metadata;
      }
    }
    const auto magnitude = from.offset < 0 ? static_cast<std::uint64_t>(-(from.offset + 1)) + 1
                                           : static_cast<std::uint64_t>(from.offset);
    if ((from.offset < 0 && magnitude > base)
        || (from.offset >= 0
            && magnitude > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - base))
      co_return std::unexpected{make_error(EINVAL)};
    state->cursor->position = from.offset < 0 ? base - magnitude : base + magnitude;
    co_return state->cursor->position;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}

/**
 *  @brief 在一个 File 租约内读取属性并提取所需结果；原生 STATX 不经过文件服务。
 * @param projection 在 CQE 或 fstat 完成后转换独立持有的属性，例如权限或完整快照。
 */
template <class F>
task<expected<std::invoke_result_t<F&, const Metadata&>>> file_stat(
    std::shared_ptr<file_state> state, F projection) {
  using Result = std::invoke_result_t<F&, const Metadata&>;
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
#if defined(__linux__)
  if (io::supports_native(state->context, io::detail::operation_kind::statx)) {
    auto attributes = co_await native_metadata_awaiter{
        state->context, state->descriptor.load(), "", AT_EMPTY_PATH};
    if (!attributes)
      co_return std::unexpected{attributes.error()};
    co_return std::invoke(projection, *attributes);
  }
#endif
  co_return co_await blocking::call(
      state->context, [state, projection = std::move(projection)]() mutable -> expected<Result> {
        struct stat attributes{};
        if (::fstat(state->descriptor, &attributes))
          return std::unexpected{make_error(errno)};
        return std::invoke(projection, Metadata{attributes});
      });
}

/**  @brief 使用原生 FTRUNCATE 或有界回退调用，活跃写租约持续到操作实际完成。 */
inline task<expected<void>> truncate_file(std::shared_ptr<file_state> state, std::uint64_t length) {
  auto lease = file_lease::acquire(state, true);
  if (!lease)
    co_return std::unexpected{lease.error()};
  if (!valid_offset(length))
    co_return std::unexpected{make_error(EOVERFLOW)};
  io::detail::io_request request;
  request.kind = io::detail::operation_kind::ftruncate;
  request.fd = state->descriptor.load();
  request.offset = length;
  auto result = co_await execute_file_request(
      state->context, std::move(request), [state, length]() -> expected<void> {
        if (::ftruncate(state->descriptor, static_cast<off_t>(length)))
          return std::unexpected{make_error(errno)};
        return {};
      });
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}

/** @brief 等待所需文件操作后同步文件数据或元数据。 */
inline task<expected<void>> sync_file(std::shared_ptr<file_state> state,
                                      bool data_only,
                                      bool flush_only = false) {
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
  active_file_operations::snapshot_type writes;
  {
    std::lock_guard lock(state->mutex);
    writes = state->active.capture(true);
  }
  for (const auto& write : writes)
    co_await write->completion.wait();
  if (flush_only)
    co_return expected<void>{};
  io::detail::io_request request;
  request.kind = io::detail::operation_kind::fsync;
  request.fd = state->descriptor.load();
  request.flags = data_only ? 1 : 0;
  auto synced = co_await execute_file_request(
      state->context, std::move(request), [state, data_only]() -> expected<void> {
        return sync_descriptor(state->descriptor, data_only);
      });
  if (!synced)
    co_return std::unexpected{synced.error()};
  co_return expected<void>{};
}

/** @brief 停止接纳文件操作并等待已接受请求安全完成。 */
inline task<expected<void>> close_file(std::shared_ptr<file_state> state) {
  if (!state)
    co_return expected<void>{};
  active_file_operations::snapshot_type pending;
  bool owner = false;
  {
    std::lock_guard lock(state->mutex);
    if (!state->closing) {
      pending = state->active.capture();
      owner = true;
      state->closing = true;
      state->context.domain()->retain_cleanup_wait();
    }
  }
  if (!owner) {
    co_await state->close_completion.wait();
    if (state->close_error)
      co_return std::unexpected{*state->close_error};
    co_return expected<void>{};
  }
  for (const auto& operation : pending)
    co_await operation->completion.wait();
  state->context.unregister_shutdown_cleanup(state->shutdown_registration);
  const int fd = state->descriptor.exchange(-1);
  expected<void> result;
  if (fd >= 0) {
    io::detail::io_request request;
    request.kind = io::detail::operation_kind::close;
    request.fd = fd;
    auto closed = co_await execute_file_request(
        state->context,
        std::move(request),
        [fd]() -> expected<void> {
          if (::close(fd))
            return std::unexpected{make_error(errno)};
          return {};
        },
        false);
    if (!closed)
      result = std::unexpected{closed.error()};
  }
  if (!result)
    state->close_error = result.error();
  state->close_completion.notify();
  state->context.domain()->release_cleanup_wait();
  co_return result;
}

/** @brief 复制 iovec 描述表，数据缓冲区仍由调用方持有。 */
inline expected<std::vector<iovec>> copy_vectors(std::span<const iovec> input);

/** @brief 执行文件的分散聚集读写并维护游标及操作租约。 */
inline task<expected<std::size_t>> vectored_file(std::shared_ptr<file_state> state,
                                                 expected<std::vector<iovec>> vectors,
                                                 std::optional<std::uint64_t> offset,
                                                 bool write);
}  // namespace detail

/**
 *  @brief
 * 持有 POSIX 文件的异步对象；原生 Proactor 使用磁盘操作码，否则交由独立的文件服务执行。
 * @details 普通 read/write/seek 使用共享的逻辑游标。read_at/write_at 不改变
 * 逻辑游标，可以并行。移动对象不影响已开始的操作。析构异步清理，不隐式 fsync。
 */
class File {
 public:
  /** @brief 构造未打开的空文件对象。 */
  File() = default;

  /** @brief 保存文件的共享状态与关闭责任。 */
  explicit File(std::shared_ptr<detail::file_state> state) noexcept : state_(std::move(state)) {}

  /** @brief 保存文件的共享状态与关闭责任。 */
  File(File&&) noexcept = default;

  /** @brief 转移文件资源和状态的所有权。 */
  File& operator=(File&&) noexcept = default;

  /** @brief 禁止复制此对象。 */
  File(const File&) = delete;

  /** @brief 禁止复制赋值。 */
  File& operator=(const File&) = delete;

  /** @brief 按打开选项异步打开文件，并关联 I/O 上下文。 */
  static task<expected<File>> open(io::io_context context, path filename, OpenOptions options) {
    auto flags = options.flags();
    if (!flags)
      co_return std::unexpected{flags.error()};
    if (!io::supports_native(context, io::detail::operation_kind::open)) {
      co_return co_await blocking::call(
          context,
          [context, filename = std::move(filename), options, flags = *flags]() -> expected<File> {
            int fd;
            do {
              fd = ::open(
                  filename.c_str(), flags, static_cast<mode_t>(options.permissions().bits()));
            } while (fd < 0 && errno == EINTR);
            if (fd < 0)
              return std::unexpected{make_error(errno)};
            try {
              auto state =
                  std::make_shared<detail::file_state>(context, fd, options.append_enabled());
              fd = -1;
              detail::register_file_shutdown(state);
              return File{std::move(state)};
            } catch (...) {
              if (fd >= 0)
                ::close(fd);
              throw;
            }
          });
    }
    io::detail::io_request request;
    request.kind = io::detail::operation_kind::open;
    request.fd = AT_FDCWD;
    request.path = filename.native();
    request.flags = *flags;
    request.argument = static_cast<int>(options.permissions().bits());
    auto opened = co_await io::native_file_op(context, std::move(request));
    if (!opened)
      co_return std::unexpected{opened.error()};
    int descriptor = static_cast<int>(*opened);
    try {
      auto state =
          std::make_shared<detail::file_state>(context, descriptor, options.append_enabled());
      descriptor = -1;
      detail::register_file_shutdown(state);
      co_return File{std::move(state)};
    } catch (const std::bad_alloc&) {
      if (descriptor >= 0)
        if (!context.domain()->defer_native_close(descriptor))
          context.defer_cleanup([descriptor] { ::close(descriptor); });
      co_return std::unexpected{make_error(ENOMEM)};
    } catch (const std::system_error& error) {
      if (descriptor >= 0)
        if (!context.domain()->defer_native_close(descriptor))
          context.defer_cleanup([descriptor] { ::close(descriptor); });
      co_return std::unexpected{make_error(error.code().value())};
    }
  }

  /** @brief 按打开选项异步打开文件，并关联 I/O 上下文。 */
  static auto open(io::io_context context, path filename) {
    return open(std::move(context), std::move(filename), OpenOptions{}.read());
  }

  /** @brief 按打开选项异步打开文件，并关联 I/O 上下文。 */
  static auto open(path filename, OpenOptions options = OpenOptions{}.read()) {
    return open(io::io_context::current(), std::move(filename), options);
  }

  /** @brief 以可写、创建和截断模式打开文件。 */
  static auto create(io::io_context context, path filename) {
    return open(std::move(context), std::move(filename), OpenOptions{}.write().create().truncate());
  }

  /** @brief 以可写、创建和截断模式打开文件。 */
  static auto create(path filename) {
    return create(io::io_context::current(), std::move(filename));
  }

  /** @brief 读取文件字节；缓冲区非空时，返回零表示 EOF。 */
  auto read(std::span<char> bytes) { return detail::read_file(state_, bytes, std::nullopt); }

  /** @brief 读取文件字节；缓冲区非空时，返回零表示 EOF。 */
  auto read(io::borrowed_buffer bytes) { return read(bytes.bytes); }

  /** @brief 写入文件字节并返回本次实际写入数量。 */
  auto write(std::span<const char> bytes) {
    return detail::write_file(state_, bytes, std::nullopt);
  }

  /** @brief 写入文件字节并返回本次实际写入数量。 */
  auto write(io::borrowed_const_buffer bytes) { return write(bytes.bytes); }

  /** @brief 读取文件字节；缓冲区非空时，返回零表示 EOF。 */
  auto read(io::io_buffer bytes) {
    return detail::read_owned(state_, std::move(bytes), std::nullopt);
  }

  /** @brief 写入文件字节并返回本次实际写入数量。 */
  auto write(io::io_buffer bytes) {
    return detail::write_owned(state_, std::move(bytes), std::nullopt);
  }

  /** @brief 在给定字节偏移读取文件，不改变逻辑游标。 */
  auto read_at(std::span<char> bytes, std::uint64_t offset) {
    return detail::read_file(state_, bytes, offset);
  }

  /** @brief 在给定字节偏移写入文件，不改变逻辑游标。 */
  auto write_at(std::span<const char> bytes, std::uint64_t offset) {
    return detail::write_file(state_, bytes, offset);
  }

  /** @brief 在给定字节偏移读取文件，不改变逻辑游标。 */
  auto read_at(io::io_buffer bytes, std::uint64_t offset) {
    return detail::read_owned(state_, std::move(bytes), offset);
  }

  /** @brief 在给定字节偏移写入文件，不改变逻辑游标。 */
  auto write_at(io::io_buffer bytes, std::uint64_t offset) {
    return detail::write_owned(state_, std::move(bytes), offset);
  }

  /**  @brief 分散读取，将 iovec 数组复制到请求中；所指内存须在操作排空前保持有效。 */
  auto read_vectored(std::span<const iovec> vectors) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), {}, false);
  }

  /** @brief 发起聚集写入，按实际完成字节数解释短写。 */
  auto write_vectored(std::span<const iovec> vectors) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), {}, true);
  }

  /** @brief 在给定字节偏移散布读取，不改变逻辑游标。 */
  auto read_vectored_at(std::span<const iovec> vectors, std::uint64_t offset) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), offset, false);
  }

  /** @brief 在给定字节偏移聚集写入，不改变逻辑游标。 */
  auto write_vectored_at(std::span<const iovec> vectors, std::uint64_t offset) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), offset, true);
  }

  /** @brief 按给定定位基准与偏移更新文件逻辑游标。 */
  auto seek(seek_from from) { return detail::seek_file(state_, from); }

  /** @brief 按给定定位基准与偏移更新文件逻辑游标。 */
  auto seek(std::uint64_t offset) { return seek(seek_from::start(offset)); }

  /** @brief 读满缓冲区；提前遇到 EOF 或失败时，报告已读字节数。 */
  auto read_exact(std::span<char> bytes) { return detail::transfer_exact<false>(state_, bytes); }

  /** @brief 循环写入全部字节，失败时报告传输进度。 */
  auto write_all(std::span<const char> bytes) { return detail::write_all_file(state_, bytes); }

  /** @brief 取得文件或目录项的元数据快照。 */
  auto metadata() {
    return detail::file_stat(state_, [](const Metadata& attributes) { return attributes; });
  }

  /** @brief 异步查询打开文件的权限。 */
  auto permissions() {
    return detail::file_stat(state_,
                             [](const Metadata& attributes) { return attributes.permissions(); });
  }

  /** @brief 设置文件字节长度，扩展或截断内容。 */
  auto set_len(std::uint64_t size) { return detail::truncate_file(state_, size); }

  /** @brief 设置目标文件的权限。 */
  auto set_permissions(Permissions permissions) {
    return detail::file_request(state_, [permissions](int fd) -> expected<void> {
      if (::fchmod(fd, static_cast<mode_t>(permissions.bits())))
        return std::unexpected{make_error(errno)};
      return {};
    });
  }

  /** @brief 等待已接受写操作并同步文件数据。 */
  auto sync_data() { return detail::sync_file(state_, true); }

  /** @brief 等待已接受写操作并同步文件数据和元数据。 */
  auto sync_all() { return detail::sync_file(state_, false); }

  /**  @brief 不借用用户缓冲区，等待此前的写入完成；flush 本身不保证持久化。 */
  auto flush() { return detail::sync_file(state_, false, true); }

  /** @brief 停止接纳文件操作，等待在途操作后关闭句柄。 */
  auto close() { return detail::close_file(state_); }

  /**  @brief 通过 dup 复制句柄，并共享 faio 逻辑游标；不保证操作系统游标相互独立。 */
  auto try_clone() {
    auto state = state_;
    return detail::file_request(state, [state](int fd) -> expected<File> {
      int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
      if (duplicate < 0)
        return std::unexpected{make_error(errno)};
      try {
        auto clone = std::make_shared<detail::file_state>(state->context, duplicate, state->append);
        duplicate = -1;
        clone->cursor = state->cursor;
        detail::register_file_shutdown(clone);
        return File{std::move(clone)};
      } catch (...) {
        if (duplicate >= 0)
          ::close(duplicate);
        throw;
      }
    });
  }

  /** @brief 读取原生句柄值，不在此接口转移关闭责任。 */
  [[nodiscard]] int native_handle() const noexcept {
    return state_ ? state_->descriptor.load() : -1;
  }

  /** @brief 取得当前对象关联的 IO 上下文租约。 */
  [[nodiscard]] io::io_context context() const {
    return state_ ? state_->context : io::io_context{};
  }

 private:
  std::shared_ptr<detail::file_state>
      state_; /**< 共享持有操作状态，保证其在异步执行和领取结果期间地址不变。 */
};
}  // namespace faio::fs

#include "faio/detail/fs/provider/vectored.hpp"
