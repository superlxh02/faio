#pragma once
/**
 * @file faio/detail/fs/platform/windows/file.hpp
 * @brief Windows IOCP 文件读写，以及租约和关闭协议。
 */
#include "faio/detail/blocking/call.hpp"
#include "faio/detail/fs/provider/file_request.hpp"
#include "faio/detail/fs/core/file_lease.hpp"
#include "faio/detail/fs/core/seek_from.hpp"
#include "faio/detail/fs/platform/windows/open_options.hpp"
#include "faio/detail/io/awaiter/native_file.hpp"
#include "faio/detail/io/base/buffer.hpp"
#include "faio/detail/sync/mutex.hpp"
#include <atomic>
#include <climits>
#include <memory>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace faio::fs {
namespace detail {
/** @brief 检查字节偏移是否位于平台可表示范围。 */
inline bool valid_offset(std::uint64_t value) noexcept {
  return value <= static_cast<std::uint64_t>(INT64_MAX);
}

/**
 *  @brief 原生操作码不可用时使用的有界回退调用，通过事件句柄低位阻止产生孤立的 IOCP 完成包。
 * @details 该调用仅在独立的文件服务线程上执行。已开始的操作即使收到取消，也须等待
 *          GetOverlappedResult 实际完成，再由外层 execute 桥接逻辑仲裁取消与字节进度。
 */
inline expected<std::size_t> blocking_transfer(
    HANDLE handle, void* buffer, std::size_t length, std::uint64_t offset, bool write) noexcept {
  io::windows::owned_file_handle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event)
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  OVERLAPPED operation{};
  operation.Offset = static_cast<DWORD>(offset);
  operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
  operation.hEvent = reinterpret_cast<HANDLE>(reinterpret_cast<std::uintptr_t>(event.get()) | 1);
  DWORD count = 0;
  const BOOL completed =
      write ? ::WriteFile(handle, buffer, static_cast<DWORD>(length), &count, &operation)
            : ::ReadFile(handle, buffer, static_cast<DWORD>(length), &count, &operation);
  if (!completed) {
    const DWORD error = ::GetLastError();
    if (!write && error == ERROR_HANDLE_EOF)
      return std::size_t{0};
    if (error != ERROR_IO_PENDING)
      return std::unexpected{io::windows::make_windows_error(error)};
    if (!::GetOverlappedResult(handle, &operation, &count, TRUE)) {
      const DWORD final_error = ::GetLastError();
      if (!write && final_error == ERROR_HANDLE_EOF)
        return std::size_t{0};
      return std::unexpected{io::windows::make_windows_error(final_error, count)};
    }
  }
  return static_cast<std::size_t>(count);
}

/**  @brief 持有单次读写的参数，IOCP 与服务回退路径共用同一份描述。 */
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
    return Writing ? io::detail::operation_kind::WRITE : io::detail::operation_kind::READ;
  }

  /** @brief 将紧凑请求展开为完整请求，固定地址、移交资源租约并初始化低频字段。 */
  io::detail::io_request into_request() && {
    io::detail::io_request request;
    request.kind = kind();
    request.fd = reinterpret_cast<std::intptr_t>(state->descriptor.load());
    request.native_kind = io::detail::native_handle_kind::WINDOWS_HANDLE;
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
    return blocking_transfer(state->descriptor.load(),
                             const_cast<char*>(buffer.data()),
                             std::min(buffer.size(), static_cast<std::size_t>(INT_MAX)),
                             append ? UINT64_MAX : offset,
                             Writing);
  }
};

/**  @brief 直接 awaiter 保留参数校验和零长度行为，不创建单块转发协程帧。 */
template <bool Writing>
inline auto transfer_block(std::shared_ptr<file_state> state,
                           std::span<std::conditional_t<Writing, const char, char>> buffer,
                           std::uint64_t offset,
                           bool append = false) {
  using result_type = file_byte_awaiter<file_block_operation<Writing>>;
  if (!valid_offset(offset))
    return result_type{std::unexpected{make_error(EOVERFLOW)}};
  if (buffer.empty())
    return result_type{std::size_t{0}};
  auto context = state->context;
  return result_type{std::move(context),
                     file_block_operation<Writing>{std::move(state), buffer, offset, append}};
}

/** @brief 通过文件请求读取一个有界数据块。 */
inline auto read_block(std::shared_ptr<file_state> state,
                       std::span<char> bytes,
                       std::uint64_t offset) {
  return transfer_block<false>(std::move(state), bytes, offset);
}

/** @brief 通过文件请求写入一个有界数据块。 */
inline auto write_block(std::shared_ptr<file_state> state,
                        std::span<const char> bytes,
                        std::uint64_t offset,
                        bool append) {
  return transfer_block<true>(std::move(state), bytes, offset, append);
}

/** @brief 以下组合算法在逻辑游标、进度和取消方面遵循与 Unix 接口相同的约定。 */
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
 * @brief 组合读写全程持有游标租约，将数据按每块至多 64 KiB 单独提交到文件 provider。
 * @details 成员函数返回时已复制 state，惰性 task 不借用 this，因此移动或析构 File 不会使其失效。
 * 共享同一游标的 clone、seek 和普通读写须等待整个组合操作结束；取消错误仍保留已完成的字节数。
 */
template <bool Writing>
inline task<expected<std::size_t>> transfer_exact(
    std::shared_ptr<file_state> state,
    std::span<std::conditional_t<Writing, const char, char>> buffer) {
  auto lease = file_lease::acquire(state, Writing);
  if (!lease)
    co_return std::unexpected{lease.error()};
  std::size_t total = 0;
  try {
    auto guard = co_await state->cursor->lane.scoped_lock();
    while (total < buffer.size()) {
      auto block = buffer.subspan(total, std::min<std::size_t>(65536, buffer.size() - total));
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
        co_return std::unexpected{::faio::error{
            Writing ? ::faio::error::WRITE_ZERO : ::faio::error::UNEXPECTED_EOF, total}};
      total += *result;
      co_await this_coro::yield_if_needed();
    }
    co_return total;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{::faio::error{ECANCELED, total}};
  }
}

/** @brief 循环完成文件写入，失败时保存传输进度。 */
inline task<expected<void>> write_all_file(std::shared_ptr<file_state> state,
                                           std::span<const char> buffer) {
  auto result = co_await transfer_exact<true>(std::move(state), buffer);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
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

/**  @brief 由文件租约保护的同步控制操作，仅在有界文件服务中执行。 */
template <class F>
auto file_request(std::shared_ptr<file_state> state, F function) -> task<
    expected<typename blocking::detail::unwrap_expected<std::invoke_result_t<F&, HANDLE>>::type>> {
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
  co_return co_await blocking::call(state->context,
                                    [state, function = std::move(function)]() mutable {
                                      return std::invoke(function, state->descriptor.load());
                                    });
}

/**  @brief 仅改变逻辑游标；seek(end) 查询已打开句柄长度，不重新解析路径。 */
inline task<expected<std::uint64_t>> seek_file(std::shared_ptr<file_state> state, seek_from from) {
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
  try {
    auto guard = co_await state->cursor->lane.scoped_lock();
    std::uint64_t base = 0;
    if (from.base == seek_from::origin::CURRENT)
      base = state->cursor->position;
    else if (from.base == seek_from::origin::END) {
      auto length = co_await blocking::call(state->context, [state]() -> expected<std::uint64_t> {
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(state->descriptor.load(), &size))
          return std::unexpected{io::windows::make_windows_error(::GetLastError())};
        return static_cast<std::uint64_t>(size.QuadPart);
      });
      if (!length)
        co_return std::unexpected{length.error()};
      base = *length;
    }
    const std::uint64_t magnitude = from.offset < 0
                                        ? static_cast<std::uint64_t>(-(from.offset + 1)) + 1
                                        : static_cast<std::uint64_t>(from.offset);
    if ((from.offset < 0 && magnitude > base)
        || (from.offset >= 0
            && (base > INT64_MAX || magnitude > static_cast<std::uint64_t>(INT64_MAX) - base)))
      co_return std::unexpected{make_error(EINVAL)};
    state->cursor->position = from.offset < 0 ? base - magnitude : base + magnitude;
    co_return state->cursor->position;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}

/** @brief 异步查询打开文件的原生属性。 */
template <class F>
auto file_stat(std::shared_ptr<file_state> state, F projection) {
  return file_request(std::move(state),
                      [projection = std::move(projection)](HANDLE handle) mutable
                          -> expected<std::invoke_result_t<F&, const file_metadata&>> {
                        auto attributes = metadata_handle(handle);
                        if (!attributes)
                          return std::unexpected{attributes.error()};
                        return std::invoke(projection, *attributes);
                      });
}

/**  @brief FileEndOfFileInfo 改变长度，不改变共享内核游标。 */
inline task<expected<void>> truncate_file(std::shared_ptr<file_state> state, std::uint64_t length) {
  if (!valid_offset(length))
    co_return std::unexpected{make_error(EOVERFLOW)};
  auto lease = file_lease::acquire(state, true);
  if (!lease)
    co_return std::unexpected{lease.error()};
  co_return co_await blocking::call(state->context, [state, length]() -> expected<void> {
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(length);
    if (!::SetFileInformationByHandle(
            state->descriptor.load(), FileEndOfFileInfo, &end, sizeof(end)))
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    return {};
  });
}

/**  @brief 写屏障保存已有写租约；持久化时在辅助服务中调用 FlushFileBuffers。 */
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
  for (const auto& operation : writes)
    co_await operation->completion.wait();
  if (flush_only)
    co_return expected<void>{};
  (void)data_only;
  co_return co_await blocking::call(state->context, [state]() -> expected<void> {
    if (!::FlushFileBuffers(state->descriptor.load()))
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    return {};
  });
}

/**  @brief 拒绝新请求，等待全部已有租约结束后关闭一次；并发 close 等待同一结果。 */
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
  const HANDLE handle = state->descriptor.exchange(INVALID_HANDLE_VALUE);
  auto result = co_await blocking::call_cleanup(
      state->context, [owner = state->context, handle]() -> expected<void> {
        return close_file_handle(owner, handle);
      });
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
 *  @brief Windows 文件拥有者；磁盘读写直接提交 IOCP，控制/路径操作使用隔离服务。
 * @details 普通 read/write/seek 共享逻辑游标；read_at/write_at 可并行。
 *          创建并返回的 task 已持有状态；移动或析构原 File 不影响已开始操作。
 */
class file {
 public:
  /** @brief 构造未打开的空文件对象。 */
  file() = default;

  /** @brief 保存文件的共享状态与关闭责任。 */
  explicit file(std::shared_ptr<detail::file_state> state) noexcept : state_(std::move(state)) {}

  /** @brief 保存文件的共享状态与关闭责任。 */
  file(file&&) noexcept = default;

  /** @brief 转移文件资源和状态的所有权。 */
  file& operator=(file&&) noexcept = default;

  /** @brief 禁止复制此对象。 */
  file(const file&) = delete;

  /** @brief 禁止复制赋值。 */
  file& operator=(const file&) = delete;

  /**  @brief 在有界文件服务中执行 CreateFileW，直接交付持有句柄的 File，避免取消时丢失 HANDLE。 */
  static task<expected<file>> open(io::io_context context, path filename, open_options options) {
    auto access = options.flags();
    if (!access)
      co_return std::unexpected{access.error()};
    co_return co_await blocking::call(
        context,
        [context, filename = std::move(filename), options, access = static_cast<DWORD>(*access)]()
            -> expected<file> {
          auto name = detail::native_path(filename);
          if (!name)
            return std::unexpected{name.error()};
          io::windows::owned_file_handle handle{
              ::CreateFileW(name->c_str(),
                            access,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr,
                            options.disposition(),
                            FILE_FLAG_OVERLAPPED
                                | (options.permissions().readonly() ? FILE_ATTRIBUTE_READONLY
                                                                    : FILE_ATTRIBUTE_NORMAL),
                            nullptr)};
          if (!handle)
            return std::unexpected{io::windows::make_windows_error(::GetLastError())};
          const auto attributes = detail::metadata_handle(handle.get());
          if (!attributes)
            return std::unexpected{attributes.error()};
          if (!attributes->is_file())
            return std::unexpected{make_error(EISDIR)};
          auto state = std::make_shared<detail::file_state>(
              context, handle.get(), options.append_enabled(), access);
          handle.release();
          detail::register_file_shutdown(state);
          return file{std::move(state)};
        });
  }

  /** @brief 按打开选项异步打开文件，并关联 I/O 上下文。 */
  static auto open(io::io_context context, path filename) {
    return open(std::move(context), std::move(filename), open_options{}.read());
  }

  /** @brief 按打开选项异步打开文件，并关联 I/O 上下文。 */
  static auto open(path filename, open_options options = open_options{}.read()) {
    return open(io::io_context::current(), std::move(filename), options);
  }

  /** @brief 以可写、创建和截断模式打开文件。 */
  static auto create(io::io_context context, path filename) {
    return open(
        std::move(context), std::move(filename), open_options{}.write().create().truncate());
  }

  /** @brief 以可写、创建和截断模式打开文件。 */
  static auto create(path filename) {
    return create(io::io_context::current(), std::move(filename));
  }

  /**
   *  @brief 按共享逻辑游标读取，EOF 返回零，短读取返回实际字节数。
   * @param bytes 借用的输出缓冲区，须在整个等待和取消排空期间保持有效。
   * @return 已构造的任务拥有 File 状态，调用者可随后移动 File。
   */
  auto read(std::span<char> bytes) { return detail::read_file(state_, bytes, std::nullopt); }

  /** @brief 读取文件字节；缓冲区非空时，返回零表示 EOF。 */
  auto read(io::borrowed_buffer bytes) { return read(bytes.bytes); }

  /**
   *  @brief 按共享游标写入，追加文件由内核原子选择文件末尾。
   * @param bytes 借用的输入缓冲区，在 OVERLAPPED 操作实际结束前不得修改或释放。
   */
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

  /**  @brief 按指定位置读取，不占用逻辑游标，可与其他指定位置的请求并行。 */
  auto read_at(std::span<char> bytes, std::uint64_t offset) {
    return detail::read_file(state_, bytes, offset);
  }

  /**  @brief 按指定位置写入，不改变游标；append 文件拒绝此类写入，避免语义冲突。 */
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

  /**  @brief 串行修改逻辑游标位置；位置为负或超出 INT64_MAX 时返回错误。 */
  auto seek(seek_from from) { return detail::seek_file(state_, from); }

  /** @brief 按给定定位基准与偏移更新文件逻辑游标。 */
  auto seek(std::uint64_t offset) { return seek(seek_from::start(offset)); }

  /** @brief 读满缓冲区；提前遇到 EOF 或失败时，报告已读字节数。 */
  auto read_exact(std::span<char> bytes) { return detail::transfer_exact<false>(state_, bytes); }

  /** @brief 循环写入全部字节，失败时报告传输进度。 */
  auto write_all(std::span<const char> bytes) { return detail::write_all_file(state_, bytes); }

  /**  @brief 查询所持句柄的属性快照，无需重新解析可能被重命名或替换的路径。 */
  auto metadata() {
    return detail::file_stat(state_, [](const file_metadata& attributes) { return attributes; });
  }

  /** @brief 异步查询打开文件的权限。 */
  auto permissions() {
    return detail::file_stat(
        state_, [](const file_metadata& attributes) { return attributes.permissions(); });
  }

  /**  @brief 原生 FileEndOfFileInfo 改变长度；不改动 faio 逻辑游标。 */
  auto set_len(std::uint64_t size) { return detail::truncate_file(state_, size); }

  /**  @brief 修改 Windows 只读属性，不改写 ACL 或其它属性。 */
  auto set_permissions(::faio::fs::permissions permissions) {
    return detail::file_request(state_, [permissions](HANDLE handle) {
      io::windows::owned_file_handle control{
          ::ReOpenFile(handle,
                       FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       FILE_FLAG_OVERLAPPED)};
      if (!control)
        return expected<void>{std::unexpected{io::windows::make_windows_error(::GetLastError())}};
      return detail::set_handle_permissions(control.get(), permissions);
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

  /**
   *  @brief 通过 ReOpenFile 克隆文件句柄，并共享 faio 逻辑游标。
   * @details ReOpenFile 使用原文件对象身份，无需重新解析可能已替换的路径。
   *          新 FILE_OBJECT 可独立关联 IOCP；DuplicateHandle 共用已关联对象，
   *          不能再次 CreateIoCompletionPort。关闭一个克隆不影响其它 HANDLE。
   */
  auto try_clone() {
    auto state = state_;
    return detail::file_request(state, [state](HANDLE original) -> expected<file> {
      HANDLE duplicate = ::ReOpenFile(original,
                                      state->access,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      FILE_FLAG_OVERLAPPED);
      if (!detail::valid_handle(duplicate))
        return std::unexpected{io::windows::make_windows_error(::GetLastError())};
      io::windows::owned_file_handle handle{duplicate};
      auto clone = std::make_shared<detail::file_state>(
          state->context, handle.get(), state->append, state->access);
      handle.release();
      clone->cursor = state->cursor;
      detail::register_file_shutdown(clone);
      return file{std::move(clone)};
    });
  }

  /** @brief 读取原生句柄值，不在此接口转移关闭责任。 */
  [[nodiscard]] HANDLE native_handle() const noexcept {
    return state_ ? state_->descriptor.load() : INVALID_HANDLE_VALUE;
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

#include "faio/detail/fs/platform/windows/vectored.hpp"
