#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/windows/file.hpp"
#else

#include "faio/detail/execution/execute.hpp"
#include "faio/detail/fs/native_metadata.hpp"
#include "faio/detail/fs/open_options.hpp"
#include "faio/detail/fs/provider.hpp"
#include "faio/detail/io/util/algorithms.hpp"
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
/** @brief 逻辑游标基准；位移采用有符号值以表达 current/end 的负向移动。 */
struct seek_from {
  enum class origin { start, current, end };

  origin base{origin::start};
  std::int64_t offset{};

  static seek_from start(std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      throw std::out_of_range("文件偏移超过平台范围");
    return {origin::start, static_cast<std::int64_t>(offset)};
  }

  static seek_from current(std::int64_t offset) noexcept { return {origin::current, offset}; }

  static seek_from end(std::int64_t offset) noexcept { return {origin::end, offset}; }
};

namespace detail {
/** @brief clone 共享的逻辑游标；异步锁跨 syscall 保持串行，不阻塞 worker。 */
struct cursor_state {
  sync::mutex lane;
  std::uint64_t position{};
};

struct file_state {
  std::uint64_t shutdown_registration{};  ///< 关闭后删除弱回调，重复open不累计控制面内存。
  io::io_context context;                 // 不依赖析构线程的 TLS，清理始终交给创建时的服务。
  std::atomic<int> descriptor{-1};        // 仅在所有操作租约排空后失效。
  bool append{};
  std::shared_ptr<cursor_state> cursor{std::make_shared<cursor_state>()};
  std::mutex mutex;            // 保护 admission、close 与活跃操作登记。
  std::size_t active_count{};  ///< admission与关闭使用同一锁序列化。
  bool closing{}, shutdown_pending{}, shutdown_close_started{};
  std::optional<Error> close_error;
  ::faio::detail::completion_event close_completion;

  struct active_operation {
    std::weak_ptr<::faio::detail::completion_event> completion;
    bool write;
  };

  std::vector<active_operation> active;

  file_state(io::io_context ctx, int fd, bool append_mode)
      : context(std::move(ctx)), descriptor(fd), append(append_mode) {}

  ~file_state() {
    context.unregister_shutdown_cleanup(shutdown_registration);
    // 最后一个操作 lease 已销毁才会进入这里；清理不在 worker 同步等待。
    const int fd = descriptor.exchange(-1);
    if (fd >= 0 && !(context.domain() && context.domain()->defer_native_close(fd)))
      context.defer_cleanup([fd] { ::close(fd); });
  }
};

/** @brief 停机关闭在最后一个租约释放后提交，不用辅助线程阻塞等待租约。
 * @details fd唯一exchange之后属于原生CLOSE或fallback清理任务；完成事件必须
 *          等真实CQE/close结果，不能在交接句柄时提前通知。
 */
inline void finish_shutdown_file(const std::shared_ptr<file_state>& file) noexcept {
  int fd;
  {
    std::lock_guard lock(file->mutex);
    if (!file->shutdown_pending || file->active_count || file->shutdown_close_started)
      return;
    file->shutdown_close_started = true;
    fd = file->descriptor.exchange(-1);
  }
  file->context.unregister_shutdown_cleanup(file->shutdown_registration);
  if (fd < 0) {
    file->close_completion.notify();
    file->context.domain()->release_cleanup_wait();
    return;
  }
  if (file->context.domain() && file->context.domain()->defer_native_close(fd, [file](int error) {
        if (error)
          file->close_error = make_error(error);
        file->close_completion.notify();
        file->context.domain()->release_cleanup_wait();
      }))
    return;  // 原生控制记录拥有file租约至最终CQE，不创建清理线程job。
  file->context.defer_cleanup([file, fd] {
    if (::close(fd))
      file->close_error = make_error(errno);
    file->close_completion.notify();
    file->context.domain()->release_cleanup_wait();
  });
}

/** @brief 注册弱停机回调，不形成 File/domain 引用环。 */
inline void register_file_shutdown(const std::shared_ptr<file_state>& state) {
  std::weak_ptr<file_state> weak = state;
  state->shutdown_registration = state->context.register_shutdown_cleanup([weak] {
    if (auto file = weak.lock()) {
      {
        std::lock_guard lock(file->mutex);
        if (file->closing)
          return;  // 已有显式close负责排空和提交，不争抢其关闭责任。
        file->closing = true;
        file->shutdown_pending = true;
        file->context.domain()->retain_cleanup_wait();  // 等最后lease期间仍计入drain。
      }
      finish_shutdown_file(file);
    }
  });
}

/** @brief 操作 admission 租约；析构发布 quiescent，供 sync/close 安全等待。 */
class file_lease {
 public:
  file_lease(std::shared_ptr<file_state> state,
             std::shared_ptr<::faio::detail::completion_event> completion) noexcept
      : state_(std::move(state)), completion_(std::move(completion)) {}

  file_lease(file_lease&&) = default;

  file_lease(const file_lease&) = delete;

  ~file_lease() {
    if (!completion_)
      return;
    bool shutdown_ready;
    {
      // 系统调用及游标更新已结束，清理线程才可关闭 fd。
      std::lock_guard lock(state_->mutex);
      --state_->active_count;
      shutdown_ready = state_->shutdown_pending && !state_->active_count;
    }
    if (shutdown_ready)
      finish_shutdown_file(state_);  // 最后一条IO实际完成后才允许停机CLOSE。
    completion_->notify();           // close/flush 的协程观察同一排空点。
  }

  static expected<file_lease> acquire(const std::shared_ptr<file_state>& state,
                                      bool write = false) {
    if (!state)
      return std::unexpected{make_error(EBADF)};
    auto event = std::make_shared<::faio::detail::completion_event>();
    std::lock_guard lock(state->mutex);
    if (state->closing || state->context.stopped() || state->descriptor < 0)
      return std::unexpected{make_error(EBADF)};
    // 清除已经完成的弱引用，内存上限由当前已接受操作数决定。
    std::erase_if(state->active,
                  [](const auto& operation) { return operation.completion.expired(); });
    state->active.push_back({event, write});
    ++state->active_count;
    return file_lease{state, std::move(event)};
  }

 private:
  std::shared_ptr<file_state> state_;
  std::shared_ptr<::faio::detail::completion_event> completion_;
};

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

/** @brief 单块文件读取：uring原生READ，epoll/kqueue走有界pread服务。 */
inline task<expected<std::size_t>> read_block(std::shared_ptr<file_state> state,
                                              std::span<char> buffer,
                                              std::uint64_t offset) {
  if (!valid_offset(offset))
    co_return std::unexpected{make_error(EOVERFLOW)};
  if (buffer.empty())
    co_return std::size_t{0};
  io::detail::io_request request;
  request.kind = io::detail::operation_kind::read;  // 原生路径无需把普通文件挂到reactor。
  request.fd = state->descriptor.load();            // 外层活跃租约保证直到CQE/服务完成都不会关闭。
  request.buffer = buffer.data();
  request.length = std::min(buffer.size(), static_cast<std::size_t>(INT_MAX));
  request.offset = offset;  // 始终使用显式偏移，普通读也由faio逻辑游标控制。
  auto result =
      co_await execute_file_request(state->context, std::move(request), [state, buffer, offset] {
        return positional_read(state->descriptor, buffer, offset);
      });
  co_return file_byte_count(std::move(result));
}

/** @brief 单块文件写入；append用流式offset保持O_APPEND内核追加原子性。 */
inline task<expected<std::size_t>> write_block(std::shared_ptr<file_state> state,
                                               std::span<const char> buffer,
                                               std::uint64_t offset,
                                               bool append) {
  if (!valid_offset(offset))
    co_return std::unexpected{make_error(EOVERFLOW)};
  if (buffer.empty())
    co_return std::size_t{0};
  io::detail::io_request request;
  request.kind = io::detail::operation_kind::write;
  request.fd = state->descriptor.load();
  request.const_buffer = buffer.data();
  request.length = std::min(buffer.size(), static_cast<std::size_t>(INT_MAX));
  request.offset = append ? UINT64_MAX : offset;
  auto result = co_await execute_file_request(
      state->context, std::move(request), [state, buffer, offset, append] {
        return positional_write(state->descriptor, buffer, offset, append);
      });
  co_return file_byte_count(std::move(result));
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
    // 普通 read/seek 串行管理逻辑位置；read_at 不占用该游标 lane。
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
  auto lease = file_lease::acquire(state, Writing);  // close等待整个组合操作，而不是单个块。
  if (!lease)
    co_return std::unexpected{lease.error()};
  std::size_t total = 0;  // 已完成的进度只由持有游标锁的本协程更新。
  try {
    auto guard = co_await state->cursor->lane.scoped_lock();  // 跨所有短IO保留游标顺序。
    while (total < buffer.size()) {
      auto block = buffer.subspan(total, std::min<std::size_t>(65536, buffer.size() - total));
      expected<std::size_t> result;
      if constexpr (Writing)
        result = co_await write_block(state, block, state->cursor->position, state->append);
      else
        result = co_await read_block(state, block, state->cursor->position);
      // 原生CQE或fallback完成以后才推进游标，worker不阻塞等待磁盘。
      const auto progress = result ? *result : result.error().progress();
      state->cursor->position += progress;  // 即使取消晚于syscall，也保留真实游标变化。
      if (!result)
        co_return std::unexpected{Error{result.error().value(), total + progress}};
      if (!*result)
        co_return std::unexpected{Error{Writing ? Error::WriteZero : Error::UnexpectedEOF, total}};
      total += *result;                       // 允许短IO，下一轮只请求尚未传输的后缀。
      co_await this_coro::yield_if_needed();  // 长文件复制不能无限占用worker协作预算。
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
        // 当前seek租约已保护fd，无需再次取得File租约或借助文件服务。
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
  std::vector<std::shared_ptr<::faio::detail::completion_event>> writes;
  {
    std::lock_guard lock(state->mutex);
    // 屏障只等待调用前已经登记的写入；后来提交的写入不改变快照。
    for (const auto& operation : state->active)
      if (operation.write)
        if (auto event = operation.completion.lock())
          writes.push_back(std::move(event));
  }
  for (const auto& write : writes)
    co_await write->wait();
  if (flush_only)
    co_return expected<void>{};
  io::detail::io_request request;
  request.kind = io::detail::operation_kind::fsync;
  request.fd = state->descriptor.load();
  request.flags = data_only ? 1 : 0;  // 中立FDATASYNC位与uring FSYNC_DATASYNC对应。
  auto synced = co_await execute_file_request(
      state->context, std::move(request), [state, data_only]() -> expected<void> {
        int result;
        do {
#if defined(__APPLE__)
          (void)data_only;
          // macOS 没有 POSIX fdatasync，用 fsync 提供不少于请求的持久化保证。
          result = ::fsync(state->descriptor);
#else
                        result = data_only
                                     ? ::fdatasync(state->descriptor)
                                     : ::fsync(state->descriptor);
#endif
        } while (result < 0 && errno == EINTR);
        if (result < 0)
          return std::unexpected{make_error(errno)};
        return {};
      });
  if (!synced)
    co_return std::unexpected{synced.error()};
  co_return expected<void>{};
}

inline task<expected<void>> close_file(std::shared_ptr<file_state> state) {
  if (!state)
    co_return expected<void>{};
  std::vector<std::shared_ptr<::faio::detail::completion_event>> pending;
  bool owner = false;
  {
    std::lock_guard lock(state->mutex);
    if (!state->closing) {
      for (const auto& operation : state->active)
        if (auto event = operation.completion.lock())
          pending.push_back(std::move(event));
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
  // completion_event 不可取消：close 必须把已接受借用 IO 安全排空。
  for (const auto& event : pending)
    co_await event->wait();
  state->context.unregister_shutdown_cleanup(state->shutdown_registration);
  const int fd = state->descriptor.exchange(-1);  // 唯一接管点，析构和停机不能重复关闭。
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
        false);  // 已接受close不可被父stop取消，等待真正CQE或清理服务完成。
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
}  // namespace detail

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
      // fallback直接交付拥有型File；若execute在完成仲裁时取消，结果析构仍负责close。
      // 不能只返回裸fd整数，否则“open成功但取消先赢”会丢失新fd的关闭责任。
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
              fd = -1;  // 控制块接管以后异常路径不能再次close。
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
    request.path = filename.native();  // 原生请求拥有路径，SQE不能指向临时字符串。
    request.flags = *flags;
    request.argument = static_cast<int>(options.permissions().bits());
    auto opened = co_await io::native_file_op(context, std::move(request));
    if (!opened)
      co_return std::unexpected{opened.error()};
    int descriptor = static_cast<int>(*opened);
    try {
      auto state =
          std::make_shared<detail::file_state>(context, descriptor, options.append_enabled());
      descriptor = -1;  // 控制块已接管fd，异常路径不能再次close。
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

  auto read(std::span<char> bytes) { return detail::read_file(state_, bytes, std::nullopt); }

  auto read(io::borrowed_buffer bytes) { return read(bytes.bytes); }

  auto write(std::span<const char> bytes) {
    return detail::write_file(state_, bytes, std::nullopt);
  }

  auto write(io::borrowed_const_buffer bytes) { return write(bytes.bytes); }

  auto read(io::io_buffer bytes) {
    return detail::read_owned(state_, std::move(bytes), std::nullopt);
  }

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

  auto write_vectored(std::span<const iovec> vectors) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), {}, true);
  }

  auto read_vectored_at(std::span<const iovec> vectors, std::uint64_t offset) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), offset, false);
  }

  auto write_vectored_at(std::span<const iovec> vectors, std::uint64_t offset) {
    return detail::vectored_file(state_, detail::copy_vectors(vectors), offset, true);
  }

  auto seek(seek_from from) { return detail::seek_file(state_, from); }

  auto seek(std::uint64_t offset) { return seek(seek_from::start(offset)); }

  auto read_exact(std::span<char> bytes) { return detail::transfer_exact<false>(state_, bytes); }

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

  [[nodiscard]] int native_handle() const noexcept {
    return state_ ? state_->descriptor.load() : -1;
  }

  [[nodiscard]] io::io_context context() const {
    return state_ ? state_->context : io::io_context{};
  }

 private:
  std::shared_ptr<detail::file_state> state_;
};
}  // namespace faio::fs

#include "faio/detail/fs/vectored.hpp"

#endif  // _WIN32
