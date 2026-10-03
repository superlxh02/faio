#pragma once
/** @file directory.hpp @brief Windows 有界目录迭代与取消/关闭租约。 */
#include "faio/detail/fs/windows/operations.hpp"
#include <deque>

namespace faio::fs {
/** @brief 拥有型 UTF-16 条目；metadata 按需获取，不为整个目录预先 stat。 */
class DirEntry {
 public:
  DirEntry(io::io_context context, fs::path filename, DWORD attributes)
      : context_(std::move(context)), path_(std::move(filename)), attributes_(attributes) {}

  fs::path path() const { return path_; }

  fs::path file_name() const { return path_.filename(); }

  auto metadata() const { return fs::metadata(context_, path_); }

  auto file_type() const { return determine_type(context_, path_, attributes_); }

 private:
  static task<expected<std::filesystem::file_type>> determine_type(io::io_context context,
                                                                   fs::path filename,
                                                                   DWORD attributes) {
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      co_return attributes& FILE_ATTRIBUTE_DIRECTORY ? std::filesystem::file_type::directory
                                                     : std::filesystem::file_type::regular;
    auto value = co_await fs::symlink_metadata(std::move(context), std::move(filename));
    if (!value)
      co_return std::unexpected{value.error()};
    co_return value->file_type();
  }

  io::io_context context_;
  fs::path path_;
  DWORD attributes_{};
};

namespace detail {
/** @brief 一个目录枚举 lane，最多缓存 64 项或 64 KiB 名称。 */
struct directory_state {
  std::uint64_t shutdown_registration{};
  io::io_context context;
  fs::path path;
  std::shared_ptr<native_directory_cursor> cursor;
  std::mutex mutex;  ///< admission 和 close 短锁，原生调用从不占有此锁。
  bool busy{}, closing{}, eof{}, shutdown_pending{}, shutdown_close_started{};
  std::optional<Error> error, close_error;
  ::faio::detail::completion_event close_completion;
  std::deque<DirEntry> entries;
  std::shared_ptr<::faio::detail::completion_event> pending;

  directory_state(io::io_context owner,
                  fs::path name,
                  std::shared_ptr<native_directory_cursor> native)
      : context(std::move(owner)), path(std::move(name)), cursor(std::move(native)) {}

  ~directory_state() { context.unregister_shutdown_cleanup(shutdown_registration); }
};

/** @brief 当前枚举完成才接管目录 HANDLE；不用辅助线程阻塞等待 busy。 */
inline void finish_shutdown_directory(const std::shared_ptr<directory_state>& state) noexcept {
  HANDLE handle;
  {
    std::lock_guard lock(state->mutex);
    if (!state->shutdown_pending || state->busy || state->shutdown_close_started)
      return;
    state->shutdown_close_started = true;
    handle = state->cursor->release();
  }
  state->context.unregister_shutdown_cleanup(state->shutdown_registration);
  state->context.defer_cleanup([state, handle] {
    if (valid_handle(handle) && !::CloseHandle(handle))
      state->close_error = io::windows::make_windows_error(::GetLastError());
    state->close_completion.notify();
    state->context.domain()->release_cleanup_wait();
  });
}

inline void register_directory_shutdown(const std::shared_ptr<directory_state>& state) {
  std::weak_ptr<directory_state> weak = state;
  state->shutdown_registration = state->context.register_shutdown_cleanup([weak] {
    if (auto directory = weak.lock()) {
      {
        std::lock_guard lock(directory->mutex);
        if (directory->closing)
          return;
        directory->closing = true;
        directory->shutdown_pending = true;
        directory->context.domain()->retain_cleanup_wait();
      }
      finish_shutdown_directory(directory);
    }
  });
}

/** @brief 枚举 lease 覆盖原生填充和缓存修改；close 等待其完成事件。 */
struct directory_lease {
  std::shared_ptr<directory_state> state;
  std::shared_ptr<::faio::detail::completion_event> completion;

  ~directory_lease() {
    bool shutdown_ready;
    {
      std::lock_guard lock(state->mutex);
      state->busy = false;
      shutdown_ready = state->shutdown_pending;
    }
    if (shutdown_ready)
      finish_shutdown_directory(state);
    completion->notify();
  }
};

inline task<expected<std::optional<DirEntry>>> next_entry(std::shared_ptr<directory_state> state) {
  if (!state)
    co_return std::unexpected{make_error(EBADF)};
  auto completion = std::make_shared<::faio::detail::completion_event>();
  {
    std::lock_guard lock(state->mutex);
    if (state->closing || state->context.stopped())
      co_return std::unexpected{make_error(EBADF)};
    if (state->busy)
      co_return std::unexpected{make_error(EBUSY)};
    state->busy = true;
    state->pending = completion;
  }
  directory_lease lease{state, completion};
  if (state->entries.empty() && !state->eof) {
    auto result = co_await execution::execute(state->context, [state]() -> expected<void> {
      std::size_t count = 0, bytes = 0;
      while (count < 64 && bytes < 65536) {
        auto entry = state->cursor->take();
        if (!entry) {
          state->error = entry.error();
          state->eof = true;
          break;
        }
        if (!*entry) {
          if (state->cursor->eof) {
            state->eof = true;
            break;
          }
          const auto filled = state->cursor->fill();
          if (!filled) {
            state->error = filled.error();
            state->eof = true;
            break;
          }
          continue;
        }
        auto& item = **entry;
        bytes += item.name.size() * sizeof(wchar_t);  // 资源预算按真实名称存储字节计算。
        state->entries.emplace_back(
            state->context, state->path / fs::path{std::move(item.name)}, item.attributes);
        ++count;
      }
      return {};
    });
    if (!result)
      co_return std::unexpected{result.error()};  // 取消后已枚举缓存仍保留可领取。
  }
  if (!state->entries.empty()) {
    auto entry = std::move(state->entries.front());
    state->entries.pop_front();
    co_return std::optional<DirEntry>{std::move(entry)};
  }
  if (state->error)
    co_return std::unexpected{*state->error};
  co_return std::optional<DirEntry>{};
}

/** @brief 并发 close 共享完成事件，真实关闭之前等待最后枚举 lease 排空。 */
inline task<expected<void>> close_directory(std::shared_ptr<directory_state> state) {
  if (!state)
    co_return expected<void>{};
  std::shared_ptr<::faio::detail::completion_event> pending;
  bool owner = false;
  {
    std::lock_guard lock(state->mutex);
    if (!state->closing) {
      owner = true;
      state->closing = true;
      state->context.domain()->retain_cleanup_wait();
      if (state->busy)
        pending = state->pending;
    }
  }
  if (!owner) {
    co_await state->close_completion.wait();
    if (state->close_error)
      co_return std::unexpected{*state->close_error};
    co_return expected<void>{};
  }
  if (pending)
    co_await pending->wait();
  state->context.unregister_shutdown_cleanup(state->shutdown_registration);
  const HANDLE handle = state->cursor->release();
  auto result = co_await execution::execute_cleanup(state->context, [handle]() -> expected<void> {
    if (valid_handle(handle) && !::CloseHandle(handle))
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    return {};
  });
  if (!result)
    state->close_error = result.error();
  state->close_completion.notify();
  state->context.domain()->release_cleanup_wait();
  co_return result;
}
}  // namespace detail

/** @brief 移动型、有界异步目录迭代器；不提供目录内容快照语义。 */
class ReadDir {
 public:
  explicit ReadDir(std::shared_ptr<detail::directory_state> state) : state_(std::move(state)) {}

  ReadDir(ReadDir&&) noexcept = default;

  ReadDir& operator=(ReadDir&&) noexcept = default;

  ReadDir(const ReadDir&) = delete;

  ReadDir& operator=(const ReadDir&) = delete;

  auto next_entry() { return detail::next_entry(state_); }

  auto close() { return detail::close_directory(state_); }

 private:
  std::shared_ptr<detail::directory_state> state_;
};

inline task<expected<ReadDir>> read_dir(io::io_context context, fs::path filename) {
  co_return co_await execution::execute(
      context, [context, filename = std::move(filename)]() -> expected<ReadDir> {
        auto handle =
            detail::open_path_handle(filename, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
        if (!handle)
          return std::unexpected{handle.error()};
        const auto attributes = detail::metadata_handle(handle->get());
        if (!attributes)
          return std::unexpected{attributes.error()};
        if (!attributes->is_dir())
          return std::unexpected{make_error(ENOTDIR)};
        auto cursor = std::make_shared<detail::native_directory_cursor>(context, handle->get());
        handle->release();
        auto state = std::make_shared<detail::directory_state>(
            context, std::move(filename), std::move(cursor));
        detail::register_directory_shutdown(state);
        return ReadDir{std::move(state)};
      });
}

inline auto read_dir(fs::path filename) {
  return read_dir(io::io_context::current(), std::move(filename));
}
}  // namespace faio::fs
