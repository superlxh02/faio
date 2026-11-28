#pragma once
/**
 * @file faio/detail/fs/platform/windows/directory.hpp
 * @brief Windows 有界目录迭代，以及取消和关闭租约。
 */
#include "faio/detail/fs/platform/windows/operations.hpp"
#include <deque>

namespace faio::fs {
/**  @brief 独立持有 UTF-16 目录项；按需获取 metadata，无需预先查询整个目录的属性。 */
class dir_entry {
 public:
  /** @brief 保存目录项的名称、路径和文件类型。 */
  dir_entry(io::io_context context, fs::path filename, DWORD attributes)
      : context_(std::move(context)), path_(std::move(filename)), attributes_(attributes) {}

  /** @brief 返回对象保存的文件系统路径。 */
  fs::path path() const { return path_; }

  /** @brief 返回目录项的末级文件名。 */
  fs::path file_name() const { return path_.filename(); }

  /** @brief 取得文件或目录项的元数据快照。 */
  auto metadata() const { return fs::metadata(context_, path_); }

  /** @brief 返回文件或目录项的类型。 */
  auto file_type() const { return determine_type(context_, path_, attributes_); }

 private:
  /** @brief 从目录枚举信息提取文件类型。 */
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

  io::io_context context_; /**< 持有所属 I/O 上下文租约，用于确定资源归属、选择服务并安全排空。 */
  fs::path path_;          /**< 持有文件系统路径，保证其在异步提交和执行期间有效。 */
  DWORD attributes_{};     /**< Windows 原生文件属性，用于分类与权限映射。 */
};

namespace detail {
/**  @brief 串行目录枚举状态，最多缓存 64 个条目或 64 KiB 的名称。 */
struct directory_state {
  std::uint64_t shutdown_registration{}; /**< 向 IO 域登记的停机清理编号，正常关闭时撤销。 */
  io::io_context context; /**< 持有所属 I/O 上下文租约，用于确定资源归属、选择服务并安全排空。 */
  fs::path path;          /**< 持有文件系统路径，保证其在异步提交和执行期间有效。 */
  std::shared_ptr<native_directory_cursor>
      cursor;       /**< 共享文件位置或目录枚举游标，由对应的平台实现推进。 */
  std::mutex mutex; /**< 在短临界区内保护操作接纳和 close，原生调用期间不持锁。 */
  bool busy{}, closing{}, eof{}, shutdown_pending{},
      shutdown_close_started{}; /**< 记录停机关闭操作是否已启动，防止重复关闭。 */
  std::optional<::faio::error> error,
      close_error; /**< 保存关闭过程中的错误，使重复关闭的等待者取得相同结果。 */
  ::faio::detail::completion_event
      close_completion;          /**< 保留关闭完成状态的事件，所有关闭等待者共享该通知。 */
  std::deque<dir_entry> entries; /**< 已解析但尚未交付给调用者的目录项队列。 */
  std::shared_ptr<::faio::detail::completion_event>
      pending; /**< 当前在途目录批次的共享完成事件。 */

  /** @brief 保存目录句柄、路径和 I/O 上下文。 */
  directory_state(io::io_context owner,
                  fs::path name,
                  std::shared_ptr<native_directory_cursor> native)
      : context(std::move(owner)), path(std::move(name)), cursor(std::move(native)) {}

  /** @brief 回收目录枚举的原生句柄。 */
  ~directory_state() { context.unregister_shutdown_cleanup(shutdown_registration); }
};

/**  @brief 当前枚举完成后才接管目录 HANDLE，无需辅助线程阻塞等待 busy 状态解除。 */
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

/** @brief 登记目录停机清理，避免 I/O 上下文退出后遗留句柄。 */
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

/**  @brief 枚举租约持续到原生缓存填充和缓存修改结束，close 等待其完成事件。 */
struct directory_lease {
  std::shared_ptr<directory_state>
      state; /**< 共享持有操作状态，保证其在异步执行和领取结果期间地址不变。 */
  std::shared_ptr<::faio::detail::completion_event>
      completion; /**< 保留完成状态的事件；通知后才到达的等待者也可直接继续执行。 */

  /** @brief 归还目录操作租约并通知关闭等待者。 */
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

/** @brief 异步读取下一个目录项，枚举结束返回空值。 */
inline task<expected<std::optional<dir_entry>>> next_entry(std::shared_ptr<directory_state> state) {
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
    auto result = co_await blocking::call(state->context, [state]() -> expected<void> {
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
        bytes += item.name.size() * sizeof(wchar_t);
        state->entries.emplace_back(
            state->context, state->path / fs::path{std::move(item.name)}, item.attributes);
        ++count;
      }
      return {};
    });
    if (!result)
      co_return std::unexpected{result.error()};
  }
  if (!state->entries.empty()) {
    auto entry = std::move(state->entries.front());
    state->entries.pop_front();
    co_return std::optional<dir_entry>{std::move(entry)};
  }
  if (state->error)
    co_return std::unexpected{*state->error};
  co_return std::optional<dir_entry>{};
}

/**  @brief 并发 close 共享完成事件，实际关闭前等待最后一个枚举租约结束。 */
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
  auto result = co_await blocking::call_cleanup(state->context, [handle]() -> expected<void> {
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

/**  @brief 只可移动的有界异步目录迭代器，不提供目录内容的快照语义。 */
class directory_reader {
 public:
  /** @brief 持有目录枚举的共享状态。 */
  explicit directory_reader(std::shared_ptr<detail::directory_state> state)
      : state_(std::move(state)) {}

  /** @brief 转移目录枚举状态和资源租约。 */
  directory_reader(directory_reader&&) noexcept = default;

  /** @brief 转移目录枚举资源和状态的所有权。 */
  directory_reader& operator=(directory_reader&&) noexcept = default;

  /** @brief 禁止复制此对象。 */
  directory_reader(const directory_reader&) = delete;

  /** @brief 禁止复制赋值。 */
  directory_reader& operator=(const directory_reader&) = delete;

  /** @brief 异步读取下一个目录项，枚举结束返回空值。 */
  auto next_entry() { return detail::next_entry(state_); }

  /** @brief 停止目录枚举，等待在途读取后关闭原生句柄。 */
  auto close() { return detail::close_directory(state_); }

 private:
  std::shared_ptr<detail::directory_state>
      state_; /**< 共享持有操作状态，保证其在异步执行和领取结果期间地址不变。 */
};

/** @brief 异步打开目录，返回目录枚举器。 */
inline task<expected<directory_reader>> read_dir(io::io_context context, fs::path filename) {
  co_return co_await blocking::call(
      context, [context, filename = std::move(filename)]() -> expected<directory_reader> {
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
        return directory_reader{std::move(state)};
      });
}

/** @brief 异步打开目录，返回目录枚举器。 */
inline auto read_dir(fs::path filename) {
  return read_dir(io::io_context::current(), std::move(filename));
}
}  // namespace faio::fs
