#pragma once
/**
 * @file faio/detail/fs/platform/windows/file.hpp
 * @brief Windows IOCP 文件读写及严格租约/关闭协议。
 */
#include "faio/detail/execution/execute.hpp"
#include "faio/detail/fs/provider/file_request.hpp"
#include "faio/detail/fs/core/file_lease.hpp"
#include "faio/detail/fs/core/seek_from.hpp"
#include "faio/detail/fs/platform/windows/open_options.hpp"
#include "faio/detail/io/awaiter/native_file.hpp"
#include "faio/detail/io/stream/algorithms.hpp"
#include "faio/detail/sync/mutex.hpp"
#include <atomic>
#include <climits>
#include <memory>
#include <mutex>

namespace faio::fs {
namespace detail {
inline bool valid_offset(std::uint64_t value) noexcept {
  return value <= static_cast<std::uint64_t>(INT64_MAX);
}

/** @brief 原生 opcode 不可用时的有界 fallback；事件低位禁止产生孤立 IOCP 包。
 * @details 该调用只运行在隔离文件服务线程。已开始操作即使收到取消也等待真实
 *          GetOverlappedResult 完成，外层 execute 桥再仲裁取消和字节进度。
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

/** @brief 单次读写的拥有参数；IOCP 与服务 fallback 共用同一份描述。 */
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
    request.fd = reinterpret_cast<std::intptr_t>(state->descriptor.load());
    request.native_kind = io::detail::native_handle_kind::windows_handle;
    if constexpr (Writing)
      request.const_buffer = buffer.data();
    else
      request.buffer = buffer.data();
    request.length = std::min(buffer.size(), static_cast<std::size_t>(INT_MAX));
    request.offset = append ? UINT64_MAX : offset;
    return request;
  }

  expected<std::size_t> operator()() const noexcept {
    return blocking_transfer(state->descriptor.load(),
                             const_cast<char*>(buffer.data()),
                             std::min(buffer.size(), static_cast<std::size_t>(INT_MAX)),
                             append ? UINT64_MAX : offset,
                             Writing);
  }
};

/** @brief 直接 awaiter 保留校验和零长度行为，不创建单块转发协程帧。 */
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
                       std::span<char> bytes,
                       std::uint64_t offset) {
  return transfer_block<false>(std::move(state), bytes, offset);
}

inline auto write_block(std::shared_ptr<file_state> state,
                        std::span<const char> bytes,
                        std::uint64_t offset,
                        bool append) {
  return transfer_block<true>(std::move(state), bytes, offset, append);
}

/// @brief 以下组合算法与已有 Unix 接口保持相同的逻辑游标/进度/取消契约。
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
        co_return std::unexpected{
            Error{result.error().value(), total + progress, result.error().domain()}};
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

/** @brief 受文件 lease 保护的同步控制操作，只在有界文件服务执行。 */
template <class F>
auto file_request(std::shared_ptr<file_state> state, F function) -> task<
    expected<typename execution::detail::unwrap_expected<std::invoke_result_t<F&, HANDLE>>::type>> {
  auto lease = file_lease::acquire(state);
  if (!lease)
    co_return std::unexpected{lease.error()};
  co_return co_await execution::execute(state->context,
                                        [state, function = std::move(function)]() mutable {
                                          return std::invoke(function, state->descriptor.load());
                                        });
}

/** @brief 仅改变逻辑游标；seek(end) 查询已打开句柄长度，不重新解析路径。 */
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
      auto length =
          co_await execution::execute(state->context, [state]() -> expected<std::uint64_t> {
            LARGE_INTEGER size{};
            if (!::GetFileSizeEx(state->descriptor.load(), &size))
              return std::unexpected{io::windows::make_windows_error(::GetLastError())};
            return static_cast<std::uint64_t>(size.QuadPart);
          });
      if (!length)
        co_return std::unexpected{length.error()};
      base = *length;
    }
    /// @brief INT64_MIN 也不发生有符号溢出。
    const std::uint64_t magnitude =
        from.offset < 0
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

template <class F>
auto file_stat(std::shared_ptr<file_state> state, F projection) {
  return file_request(std::move(state),
                      [projection = std::move(projection)](HANDLE handle) mutable
                          -> expected<std::invoke_result_t<F&, const Metadata&>> {
                        auto attributes = metadata_handle(handle);
                        if (!attributes)
                          return std::unexpected{attributes.error()};
                        return std::invoke(projection, *attributes);
                      });
}

/** @brief FileEndOfFileInfo 改变长度，不改变共享内核游标。 */
inline task<expected<void>> truncate_file(std::shared_ptr<file_state> state, std::uint64_t length) {
  if (!valid_offset(length))
    co_return std::unexpected{make_error(EOVERFLOW)};
  auto lease = file_lease::acquire(state, true);
  if (!lease)
    co_return std::unexpected{lease.error()};
  co_return co_await execution::execute(state->context, [state, length]() -> expected<void> {
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(length);
    if (!::SetFileInformationByHandle(
            state->descriptor.load(), FileEndOfFileInfo, &end, sizeof(end)))
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    return {};
  });
}

/** @brief 写屏障捕获已有写 lease，持久化操作在辅助服务调用 FlushFileBuffers。 */
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
    /// @brief 不阻塞 worker，且不受父取消跳过。
    co_await operation->completion.wait();
  /// @brief faio 没有用户态写缓存，屏障即 flush。
  if (flush_only)
    co_return expected<void>{};
  /// @brief Windows 单一接口提供不少于 sync_data 的持久化保证。
  (void)data_only;
  co_return co_await execution::execute(state->context, [state]() -> expected<void> {
    if (!::FlushFileBuffers(state->descriptor.load()))
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    return {};
  });
}

/** @brief 拒绝新请求、等待全部旧 lease、恰好一次关闭；并发 close 等同一结果。 */
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
      /// @brief 等待 lease 期间也计入域 drain。
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
    /// @brief 内核借用排空前绝不 CloseHandle。
    co_await operation->completion.wait();
  state->context.unregister_shutdown_cleanup(state->shutdown_registration);
  /// @brief 唯一转移关闭责任。
  const HANDLE handle = state->descriptor.exchange(INVALID_HANDLE_VALUE);
  auto result = co_await execution::execute_cleanup(
      state->context, [owner = state->context, handle]() -> expected<void> {
        return close_file_handle(owner, handle);
      });
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

/** @brief Windows 文件拥有者；磁盘读写直接提交 IOCP，控制/路径操作使用隔离服务。
 * @details 普通 read/write/seek 共享逻辑游标；read_at/write_at 可并行。
 *          创建返回的 task 已拥有状态；移动或析构原 File 不影响已开始操作。
 */
class File {
 public:
  File() = default;

  explicit File(std::shared_ptr<detail::file_state> state) noexcept : state_(std::move(state)) {}

  File(File&&) noexcept = default;

  File& operator=(File&&) noexcept = default;

  File(const File&) = delete;

  File& operator=(const File&) = delete;

  /** @brief CreateFileW 在有界文件服务执行；直接交付拥有型 File 防取消丢失 HANDLE。 */
  static task<expected<File>> open(io::io_context context, path filename, OpenOptions options) {
    auto access = options.flags();
    if (!access)
      co_return std::unexpected{access.error()};
    co_return co_await execution::execute(
        context,
        [context, filename = std::move(filename), options, access = static_cast<DWORD>(*access)]()
            -> expected<File> {
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
          /// @brief shared state 已成功建立后才转移唯一 HANDLE。
          handle.release();
          detail::register_file_shutdown(state);
          /// @brief 取消仲裁销毁结果也能自动清理，不泄漏裸句柄。
          return File{std::move(state)};
        });
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

  /** @brief 按共享逻辑游标读取，EOF 返回零，短读取返回实际字节数。
   * @param bytes 借用输出，必须覆盖整个等待及取消排空期间。
   * @return 已构造的任务拥有 File 状态，调用者可随后移动 File。
   */
  auto read(std::span<char> bytes) { return detail::read_file(state_, bytes, std::nullopt); }

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  auto read(io::borrowed_buffer bytes) { return read(bytes.bytes); }

  /** @brief 按共享游标写入，追加文件由内核原子选择文件末尾。
   * @param bytes 借用输入，真实 OVERLAPPED 结束前不得修改或释放。
   */
  auto write(std::span<const char> bytes) {
    return detail::write_file(state_, bytes, std::nullopt);
  }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto write(io::borrowed_const_buffer bytes) { return write(bytes.bytes); }

  /** @brief 拥有型读取；请求接管 buffer，成功返回缩减到实际大小的 buffer。 */
  auto read(io::io_buffer bytes) {
    return detail::read_owned(state_, std::move(bytes), std::nullopt);
  }

  /** @brief 拥有型写入；成功返回原 buffer 和真实写入量，调用者无需保留原容器。 */
  auto write(io::io_buffer bytes) {
    return detail::write_owned(state_, std::move(bytes), std::nullopt);
  }

  /** @brief 位置读取不占用逻辑游标 lane，可与其他位置请求并行。 */
  auto read_at(std::span<char> bytes, std::uint64_t offset) {
    return detail::read_file(state_, bytes, offset);
  }

  /** @brief 位置写入不改变游标；append 文件拒绝位置写，避免语义冲突。 */
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

  /** @brief 在逻辑游标 lane 修改位置；负位置和超出 INT64_MAX 返回明确错误。 */
  auto seek(seek_from from) { return detail::seek_file(state_, from); }

  /// @brief 按给定定位基准与偏移更新文件逻辑游标。
  auto seek(std::uint64_t offset) { return seek(seek_from::start(offset)); }

  /** @brief 读满缓冲；跨全部短 IO 保留游标，EOF/取消错误携带累计字节数。 */
  auto read_exact(std::span<char> bytes) { return detail::transfer_exact<false>(state_, bytes); }

  /** @brief 完整写出；64 KiB 批次保持协作公平性，其他 clone 不插入中间块。 */
  auto write_all(std::span<const char> bytes) { return detail::write_all_file(state_, bytes); }

  /** @brief 查询持有句柄的属性快照，不重新解析可能被重命名/替换的路径。 */
  auto metadata() {
    return detail::file_stat(state_, [](const Metadata& attributes) { return attributes; });
  }

  auto permissions() {
    return detail::file_stat(state_,
                             [](const Metadata& attributes) { return attributes.permissions(); });
  }

  /** @brief 原生 FileEndOfFileInfo 改变长度；不改动 faio 逻辑游标。 */
  auto set_len(std::uint64_t size) { return detail::truncate_file(state_, size); }

  /** @brief 修改 Windows 只读属性，不改写 ACL 或其它属性。 */
  auto set_permissions(Permissions permissions) {
    return detail::file_request(state_, [permissions](HANDLE handle) {
      /// @brief 属性写入权限由 OS 在真实对象上重新检查，读打开的 File 也可以修改其只读位。
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

  auto sync_data() { return detail::sync_file(state_, true); }

  auto sync_all() { return detail::sync_file(state_, false); }

  /** @brief 无用户缓冲；等待之前写入完成，flush 本身不要求持久化。 */
  auto flush() { return detail::sync_file(state_, false, true); }

  /// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
  auto close() { return detail::close_file(state_); }

  /** @brief ReOpenFile 克隆真实文件句柄，同时共享 faio 逻辑游标。
   * @details ReOpenFile 使用原文件对象身份，无需重新解析可能已替换的路径。
   *          新 FILE_OBJECT 可独立关联 IOCP；DuplicateHandle 共用已关联对象，
   *          不能再次 CreateIoCompletionPort。关闭一个克隆不影响其它 HANDLE。
   */
  auto try_clone() {
    auto state = state_;
    return detail::file_request(state, [state](HANDLE original) -> expected<File> {
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
      /// @brief 整个普通/组合 IO 使用同一异步游标 lane。
      clone->cursor = state->cursor;
      detail::register_file_shutdown(clone);
      return File{std::move(clone)};
    });
  }

  /// @brief 读取原生句柄值，不在此接口转移关闭责任。
  [[nodiscard]] HANDLE native_handle() const noexcept {
    return state_ ? state_->descriptor.load() : INVALID_HANDLE_VALUE;
  }

  /// @brief 取得当前对象关联的 IO 上下文租约。
  [[nodiscard]] io::io_context context() const {
    return state_ ? state_->context : io::io_context{};
  }

 private:
  std::shared_ptr<detail::file_state> state_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
};
}

#include "faio/detail/fs/platform/windows/vectored.hpp"
