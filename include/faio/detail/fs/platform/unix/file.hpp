/**
 * @file faio/detail/fs/platform/unix/file.hpp
 * @brief 文件与目录异步接口：异步文件对象。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once

#include "faio/detail/execution/execute.hpp"
#include "faio/detail/fs/core/file_lease.hpp"
#include "faio/detail/fs/core/seek_from.hpp"
#if defined(__linux__)
#include "faio/detail/fs/platform/linux/native_metadata.hpp"
#endif
#include "faio/detail/fs/open_options.hpp"
#include "faio/detail/fs/platform/unix/sync.hpp"
#include "faio/detail/fs/provider/file_request.hpp"
#include "faio/detail/io/stream/algorithms.hpp"
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

namespace faio::fs {
namespace detail {
inline bool valid_offset(std::uint64_t offset) noexcept {
  return offset <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
}

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

/** @brief 一份单块参数，原生请求和服务调用只构造被选择的一支。 */
template <bool Writing>
struct file_block_operation {
  std::shared_ptr<file_state> state;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
  std::span<std::conditional_t<Writing, const char, char>> buffer;  ///< 本次操作借用的数据区，操作安全完成前调用方须保持有效。
  std::uint64_t offset;  ///< 本次文件操作的字节偏移，定位基准由对应接口约定。
  bool append{};  ///< 是否采用追加写入语义，区别于指定偏移写入。

  static constexpr io::detail::operation_kind kind() noexcept {
    return Writing ? io::detail::operation_kind::write : io::detail::operation_kind::read;
  }

  /// @brief 把紧凑请求物化为完整稳定请求，移动资源租约并初始化冷字段。
  io::detail::io_request into_request() && {
    io::detail::io_request request;
    request.kind = kind();
    /// @brief 外层活跃租约保护 fd 和 payload 至真实完成。
    request.fd = state->descriptor.load();
    if constexpr (Writing)
      request.const_buffer = buffer.data();
    else
      request.buffer = buffer.data();
    request.length = std::min(buffer.size(), static_cast<std::size_t>(INT_MAX));
    request.offset = append ? UINT64_MAX : offset;
    return request;
  }

  expected<std::size_t> operator()() const noexcept {
    if constexpr (Writing)
      return positional_write(state->descriptor, buffer, offset, append);
    else
      return positional_read(state->descriptor, buffer, offset);
  }
};

/** @brief 单块直接 awaiter：保留校验，不为结果转换分配子协程帧。 */
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

inline auto read_block(std::shared_ptr<file_state> state,
                       std::span<char> buffer,
                       std::uint64_t offset) {
  return transfer_block<false>(std::move(state), buffer, offset);
}

/** @brief append 使用流式 offset，保持 O_APPEND 的内核追加原子性。 */
inline auto write_block(std::shared_ptr<file_state> state,
                        std::span<const char> buffer,
                        std::uint64_t offset,
                        bool append) {
  return transfer_block<true>(std::move(state), buffer, offset, append);
}

inline task<expected<std::size_t>> read_file(std::shared_ptr<file_state> state,
                                             std::span<char> buffer,
                                             std::optional<std::uint64_t> offset) {
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
  if (offset)
    co_return co_await read_block(state, buffer, *offset);
  try {
    /// @brief 普通 read/seek 串行管理逻辑位置；read_at 不占用该游标 lane。
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
 * @brief 完整组合读写持有游标租约，每个至多64KiB的块单独提交文件provider。
 * @details state在成员函数返回时已复制，移动或析构File不会使延迟task借用this。
 * 同游标的clone、seek和普通读写只能在整个组合结束以后继续；取消错误保留字节进度。
 */
template <bool Writing>
inline task<expected<std::size_t>> transfer_exact(
    std::shared_ptr<file_state> state,
    std::span<std::conditional_t<Writing, const char, char>> buffer) {
  /// @brief close等待整个组合操作，而不是单个块。
  auto lease = file_lease::acquire(state, Writing);
  if (!lease)
    co_return std::unexpected{lease.error()};
  /// @brief 已完成的进度只由持有游标锁的本协程更新。
  std::size_t total = 0;
  try {
    /// @brief 跨所有短IO保留游标顺序。
    auto guard = co_await state->cursor->lane.scoped_lock();
    while (total < buffer.size()) {
      auto block = buffer.subspan(total, std::min<std::size_t>(65536, buffer.size() - total));
      expected<std::size_t> result;
      if constexpr (Writing)
        result = co_await write_block(state, block, state->cursor->position, state->append);
      else
        result = co_await read_block(state, block, state->cursor->position);
      /// @brief 原生CQE或fallback完成以后才推进游标，worker不阻塞等待磁盘。
      const auto progress = result ? *result : result.error().progress();
      /// @brief 即使取消晚于syscall，也保留真实游标变化。
      state->cursor->position += progress;
      if (!result)
        co_return std::unexpected{Error{result.error().value(), total + progress}};
      if (!*result)
        co_return std::unexpected{Error{Writing ? Error::WriteZero : Error::UnexpectedEOF, total}};
      /// @brief 允许短IO，下一轮只请求尚未传输的后缀。
      total += *result;
      /// @brief 长文件复制不能无限占用worker协作预算。
      co_await this_coro::yield_if_needed();
    }
    co_return total;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{Error{ECANCELED, total}};
  }
}

inline task<expected<void>> write_all_file(std::shared_ptr<file_state> state,
                                           std::span<const char> buffer) {
  auto result = co_await transfer_exact<true>(std::move(state), buffer);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}

inline task<expected<io::io_transfer>> read_owned(std::shared_ptr<file_state> state,
                                                  io::io_buffer buffer,
                                                  std::optional<std::uint64_t> offset) {
  auto result = co_await read_file(std::move(state), buffer.writable_bytes(), offset);
  if (!result)
    co_return std::unexpected{result.error()};
  buffer.set_size(*result);
  co_return io::io_transfer{std::move(buffer), *result};
}

inline task<expected<io::io_transfer>> write_owned(std::shared_ptr<file_state> state,
                                                   io::io_buffer buffer,
                                                   std::optional<std::uint64_t> offset) {
  auto result = co_await write_file(std::move(state), buffer.bytes(), offset);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return io::io_transfer{std::move(buffer), *result};
}

template <class F>
auto file_request(std::shared_ptr<file_state> state, F function) -> task<
    expected<typename execution::detail::unwrap_expected<std::invoke_result_t<F&, int>>::type>> {
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
  co_return co_await execution::execute(state->context,
                                        [state, function = std::move(function)]() mutable {
                                          return std::invoke(function, state->descriptor);
                                        });
}

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
        /// @brief 当前seek租约已保护fd，无需再次取得File租约或借助文件服务。
        auto attributes = co_await native_metadata_awaiter{
            state->context, state->descriptor.load(), "", AT_EMPTY_PATH};
        if (!attributes)
          co_return std::unexpected{attributes.error()};
        base = attributes->len();
      } else
#endif
      {
        auto metadata =
            co_await execution::execute(state->context, [state]() -> expected<std::uint64_t> {
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

/** @brief 在一次File租约内读取并投影属性；原生STATX不经过文件服务。
 * @param projection 在CQE或fstat完成后转换拥有型属性，例如权限或完整快照。
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
  co_return co_await execution::execute(
      state->context, [state, projection = std::move(projection)]() mutable -> expected<Result> {
        struct stat attributes{};
        if (::fstat(state->descriptor, &attributes))
          return std::unexpected{make_error(errno)};
        return std::invoke(projection, Metadata{attributes});
      });
}

/** @brief 原生FTRUNCATE或有界fallback，活跃写租约覆盖实际完成。 */
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

inline task<expected<void>> sync_file(std::shared_ptr<file_state> state,
                                      bool data_only,
                                      bool flush_only = false) {
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
  active_file_operations::snapshot_type writes;
  {
    std::lock_guard lock(state->mutex);
    /// @brief 屏障只等待调用前已经登记的写入；后来提交的写入不改变快照。
    writes = state->active.capture(true);
  }
  for (const auto& write : writes)
    co_await write->completion.wait();
  if (flush_only)
    co_return expected<void>{};
  io::detail::io_request request;
  request.kind = io::detail::operation_kind::fsync;
  request.fd = state->descriptor.load();
  /// @brief 中立FDATASYNC位与uring FSYNC_DATASYNC对应。
  request.flags = data_only ? 1 : 0;
  auto synced = co_await execute_file_request(
      state->context, std::move(request), [state, data_only]() -> expected<void> {
        return sync_descriptor(state->descriptor, data_only);
      });
  if (!synced)
    co_return std::unexpected{synced.error()};
  co_return expected<void>{};
}

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
  /// @brief completion_event 不可取消：close 必须把已接受借用 IO 安全排空。
  for (const auto& operation : pending)
    co_await operation->completion.wait();
  state->context.unregister_shutdown_cleanup(state->shutdown_registration);
  /// @brief 唯一接管点，析构和停机不能重复关闭。
  const int fd = state->descriptor.exchange(-1);
  expected<void> result;
  if (fd >= 0) {
    io::detail::io_request request;
    request.kind = io::detail::operation_kind::close;
    request.fd = fd;
    /// @brief 已接受close不可被父stop取消，等待真正CQE或清理服务完成。
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

inline expected<std::vector<iovec>> copy_vectors(std::span<const iovec> input);

inline task<expected<std::size_t>> vectored_file(std::shared_ptr<file_state> state,
                                                 expected<std::vector<iovec>> vectors,
                                                 std::optional<std::uint64_t> offset,
                                                 bool write);
}

/** @brief
 * POSIX文件的异步拥有者；原生Proactor执行磁盘opcode，否则走隔离文件服务。
 * @details 普通 read/write/seek 使用共享的逻辑游标。read_at/write_at 不改变
 * 逻辑游标，可以并行。移动对象不影响已开始的操作。析构异步清理，不隐式 fsync。
 */
class File {
 public:
  File() = default;

  explicit File(std::shared_ptr<detail::file_state> state) noexcept : state_(std::move(state)) {}

  File(File&&) noexcept = default;

  File& operator=(File&&) noexcept = default;

  File(const File&) = delete;

  File& operator=(const File&) = delete;

  static task<expected<File>> open(io::io_context context, path filename, OpenOptions options) {
    auto flags = options.flags();
    if (!flags)
      co_return std::unexpected{flags.error()};
    if (!io::supports_native(context, io::detail::operation_kind::open)) {
      /// @brief fallback直接交付拥有型File；若execute在完成仲裁时取消，结果析构仍负责close。
      /// 不能只返回裸fd整数，否则“open成功但取消先赢”会丢失新fd的关闭责任。
      co_return co_await execution::execute(
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
              /// @brief 控制块接管以后异常路径不能再次close。
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
    /// @brief 原生请求拥有路径，SQE不能指向临时字符串。
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
      /// @brief 控制块已接管fd，异常路径不能再次close。
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

  static auto open(io::io_context context, path filename) {
    return open(std::move(context), std::move(filename), OpenOptions{}.read());
  }

  static auto open(path filename, OpenOptions options = OpenOptions{}.read()) {
    return open(io::io_context::current(), std::move(filename), options);
  }

  static auto create(io::io_context context, path filename) {
    return open(std::move(context), std::move(filename), OpenOptions{}.write().create().truncate());
  }

  static auto create(path filename) {
    return create(io::io_context::current(), std::move(filename));
  }

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  auto read(std::span<char> bytes) { return detail::read_file(state_, bytes, std::nullopt); }

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  auto read(io::borrowed_buffer bytes) { return read(bytes.bytes); }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto write(std::span<const char> bytes) {
    return detail::write_file(state_, bytes, std::nullopt);
  }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto write(io::borrowed_const_buffer bytes) { return write(bytes.bytes); }

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  auto read(io::io_buffer bytes) {
    return detail::read_owned(state_, std::move(bytes), std::nullopt);
  }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto write(io::io_buffer bytes) {
    return detail::write_owned(state_, std::move(bytes), std::nullopt);
  }

  auto read_at(std::span<char> bytes, std::uint64_t offset) {
    return detail::read_file(state_, bytes, offset);
  }

  auto write_at(std::span<const char> bytes, std::uint64_t offset) {
    return detail::write_file(state_, bytes, offset);
  }

  auto read_at(io::io_buffer bytes, std::uint64_t offset) {
    return detail::read_owned(state_, std::move(bytes), offset);
  }

  auto write_at(io::io_buffer bytes, std::uint64_t offset) {
    return detail::write_owned(state_, std::move(bytes), offset);
  }

  /** @brief 散布读取；iovec数组复制到请求，指向内存必须覆盖操作排空。 */
  auto read_vectored(std::span<const iovec> vectors) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), {}, false);
  }

  /// @brief 发起聚集写入，按实际完成字节数解释短写。
  auto write_vectored(std::span<const iovec> vectors) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), {}, true);
  }

  auto read_vectored_at(std::span<const iovec> vectors, std::uint64_t offset) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), offset, false);
  }

  auto write_vectored_at(std::span<const iovec> vectors, std::uint64_t offset) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), offset, true);
  }

  /// @brief 按给定定位基准与偏移更新文件逻辑游标。
  auto seek(seek_from from) { return detail::seek_file(state_, from); }

  /// @brief 按给定定位基准与偏移更新文件逻辑游标。
  auto seek(std::uint64_t offset) { return seek(seek_from::start(offset)); }

  /// @brief 循环读取直到缓冲区填满、EOF 或失败，失败保留已经取得的进度。
  auto read_exact(std::span<char> bytes) { return detail::transfer_exact<false>(state_, bytes); }

  /// @brief 循环写入直到全部负载完成或失败，组合期间保持方向预留。
  auto write_all(std::span<const char> bytes) { return detail::write_all_file(state_, bytes); }

  auto metadata() {
    return detail::file_stat(state_, [](const Metadata& attributes) { return attributes; });
  }

  auto permissions() {
    return detail::file_stat(state_,
                             [](const Metadata& attributes) { return attributes.permissions(); });
  }

  auto set_len(std::uint64_t size) { return detail::truncate_file(state_, size); }

  auto set_permissions(Permissions permissions) {
    return detail::file_request(state_, [permissions](int fd) -> expected<void> {
      if (::fchmod(fd, static_cast<mode_t>(permissions.bits())))
        return std::unexpected{make_error(errno)};
      return {};
    });
  }

  auto sync_data() { return detail::sync_file(state_, true); }

  auto sync_all() { return detail::sync_file(state_, false); }

  /** @brief 无用户缓冲；等待之前写入完成，flush 本身不要求持久化。 */
  auto flush() { return detail::sync_file(state_, false, true); }

  /// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
  auto close() { return detail::close_file(state_); }

  /** @brief dup 复制 handle，同时共享 faio 逻辑游标；不宣称独立 OS 游标。 */
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

  /// @brief 读取原生句柄值，不在此接口转移关闭责任。
  [[nodiscard]] int native_handle() const noexcept {
    return state_ ? state_->descriptor.load() : -1;
  }

  /// @brief 取得当前对象关联的 IO 上下文租约。
  [[nodiscard]] io::io_context context() const {
    return state_ ? state_->context : io::io_context{};
  }

 private:
  std::shared_ptr<detail::file_state> state_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
};
}

#include "faio/detail/fs/provider/vectored.hpp"
