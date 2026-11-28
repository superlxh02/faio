/**
 * @file faio/detail/fs/platform/unix/directory.hpp
 * @brief 目录项与异步枚举。
 */
#pragma once
#include "faio/detail/fs/operations.hpp"
#include <deque>
#include <dirent.h>

namespace faio::fs {
/**
 *  @brief 持有目录项路径，不借用 readdir 的临时内存；metadata 按需另行异步获取。
 */
class dir_entry {
 public:
  /** @brief 保存目录项的名称、路径和文件类型。 */
  dir_entry(io::io_context context, fs::path filename, unsigned char type)
      : context_(std::move(context)), path_(std::move(filename)), type_(type) {}

  /** @brief 返回对象保存的文件系统路径。 */
  [[nodiscard]] fs::path path() const { return path_; }

  /** @brief 返回目录项的末级文件名。 */
  [[nodiscard]] fs::path file_name() const { return path_.filename(); }

  /** @brief 取得文件或目录项的元数据快照。 */
  auto metadata() const { return fs::metadata(context_, path_); }

  /**  @brief d_type 已知时立即返回；未知时异步调用 lstat，无需预先查询整个目录的属性。 */
  auto file_type() const { return determine_type(context_, path_, type_); }

 private:
  /**  @brief 创建 task 时持有路径、context 和 type，临时 DirEntry 销毁后仍可安全等待。 */
  static task<expected<std::filesystem::file_type>> determine_type(io::io_context context,
                                                                   fs::path filename,
                                                                   unsigned char type) {
    switch (type) {
      case DT_REG:
        co_return std::filesystem::file_type::regular;
      case DT_DIR:
        co_return std::filesystem::file_type::directory;
      case DT_LNK:
        co_return std::filesystem::file_type::symlink;
      case DT_FIFO:
        co_return std::filesystem::file_type::fifo;
      case DT_SOCK:
        co_return std::filesystem::file_type::socket;
      case DT_CHR:
        co_return std::filesystem::file_type::character;
      case DT_BLK:
        co_return std::filesystem::file_type::block;
      default:
        break;
    }
    auto attributes = co_await fs::symlink_metadata(std::move(context), std::move(filename));
    if (!attributes)
      co_return std::unexpected{attributes.error()};
    co_return attributes->file_type();
  }

 private:
  io::io_context context_; /**< 持有所属 I/O 上下文租约，用于确定资源归属、选择服务并安全排空。 */
  fs::path path_;          /**< 持有文件系统路径，保证其在异步提交和执行期间有效。 */
  unsigned char type_;     /**< 目录项的原生类型提示，完整元数据查询仍以系统结果为准。 */
};

namespace detail {
/**  @brief 独占目录游标，最多缓存 64 个条目或 64 KiB 的名称；取消后仍可领取已枚举的条目。 */
struct directory_state {
  std::uint64_t shutdown_registration{}; /**< 向 IO 域登记的停机清理编号，正常关闭时撤销。 */
  io::io_context context; /**< 持有所属 I/O 上下文租约，用于确定资源归属、选择服务并安全排空。 */
  fs::path path;          /**< 持有文件系统路径，保证其在异步提交和执行期间有效。 */
  DIR* native{};          /**< 本次遍历拥有的原生目录枚举句柄。 */
#if defined(__linux__)
  std::shared_ptr<native_directory_cursor> native_cursor;
#endif
  std::mutex mutex; /**< 保护目录枚举的共享状态；调度与用户回调应遵循锁外调用协议。 */
  bool busy{}, closing{}, eof{}, shutdown_pending{},
      shutdown_close_started{};       /**< 记录停机关闭操作是否已启动，防止重复关闭。 */
  std::optional<::faio::error> error; /**< 保存操作错误，供操作完成或重复查询时返回。 */
  std::optional<::faio::error>
      close_error; /**< 保存关闭过程中的错误，使重复关闭的等待者取得相同结果。 */
  ::faio::detail::completion_event
      close_completion;          /**< 保留关闭完成状态的事件，所有关闭等待者共享该通知。 */
  std::deque<dir_entry> entries; /**< 已解析但尚未交付给调用者的目录项队列。 */
  std::shared_ptr<::faio::detail::completion_event>
      pending; /**< 当前在途目录批次的共享完成事件。 */

  /** @brief 保存目录句柄、路径和 I/O 上下文。 */
  directory_state(io::io_context owner, fs::path name, DIR* directory)
      : context(std::move(owner)), path(std::move(name)), native(directory) {}

  /** @brief 回收目录枚举的原生句柄。 */
  ~directory_state() {
    context.unregister_shutdown_cleanup(shutdown_registration);
    auto* directory = std::exchange(native, nullptr);
    if (directory)
      context.defer_cleanup([directory] { ::closedir(directory); });
  }
};

/**  @brief 在最后一个枚举租约结束后发起关闭，无需线程等待 busy 条件变量。 */
inline void finish_shutdown_directory(const std::shared_ptr<directory_state>& state) noexcept {
  DIR* directory;
  int fd = -1;
  {
    std::lock_guard lock(state->mutex);
    if (!state->shutdown_pending || state->busy || state->shutdown_close_started)
      return;
    state->shutdown_close_started = true;
    directory = std::exchange(state->native, nullptr);
#if defined(__linux__)
    if (state->native_cursor)
      fd = state->native_cursor->release();
#endif
  }
  state->context.unregister_shutdown_cleanup(state->shutdown_registration);
  if (fd >= 0 && state->context.domain()->defer_native_close(fd, [state](int error) {
        if (error)
          state->close_error = make_error(error);
        state->close_completion.notify();
        state->context.domain()->release_cleanup_wait();
      }))
    return;
  if (directory || fd >= 0) {
    state->context.defer_cleanup([state, directory, fd] {
      const int result = directory ? ::closedir(directory) : ::close(fd);
      if (result)
        state->close_error = make_error(errno);
      state->close_completion.notify();
      state->context.domain()->release_cleanup_wait();
    });
  } else {
    state->close_completion.notify();
    state->context.domain()->release_cleanup_wait();
  }
}

/**  @brief 通过弱引用登记目录清理，实际关闭由最后一个租约结束或当前空闲状态触发。 */
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

/**  @brief 请求租约保证 close 等待批量系统调用和缓存更新全部结束。 */
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
  auto event = std::make_shared<::faio::detail::completion_event>();
  {
    std::lock_guard lock(state->mutex);
    if (state->closing || state->context.stopped())
      co_return std::unexpected{make_error(EBADF)};
    if (state->busy)
      co_return std::unexpected{make_error(EBUSY)};
    state->busy = true;
    state->pending = event;
  }
  directory_lease lease{state, event};
  if (state->entries.empty() && !state->eof) {
#if defined(__linux__)
    if (state->native_cursor) {
      std::size_t count = 0, bytes = 0;
      while (count < 64 && bytes < 65536) {
        auto entry = state->native_cursor->take();
        if (!entry) {
          state->error = entry.error();
          state->eof = true;
          break;
        }
        if (!*entry) {
          if (state->native_cursor->eof) {
            state->eof = true;
            break;
          }
          auto cursor = state->native_cursor;
          auto filled =
              co_await blocking::call(state->context, [cursor] { return cursor->fill(); });
          if (!filled)
            co_return std::unexpected{filled.error()};
          continue;
        }
        bytes += (*entry)->name.size();
        state->entries.emplace_back(
            state->context, state->path / fs::path{(*entry)->name}, (*entry)->type);
        ++count;
      }
    } else
#endif
    {
      auto batch = co_await blocking::call(state->context, [state]() -> expected<void> {
        std::size_t count = 0, bytes = 0;
        while (count < 64 && bytes < 65536) {
          errno = 0;
          auto* entry = ::readdir(state->native);
          if (!entry) {
            if (errno)
              state->error = make_error(errno);
            state->eof = true;
            break;
          }
          std::string_view name{entry->d_name};
          if (name == "." || name == "..")
            continue;
          state->entries.emplace_back(state->context, state->path / fs::path{name}, entry->d_type);
          ++count;
          bytes += name.size();
        }
        return {};
      });
      if (!batch)
        co_return std::unexpected{batch.error()};
    }
  }
  if (!state->entries.empty()) {
    auto result = std::move(state->entries.front());
    state->entries.pop_front();
    co_return std::optional<dir_entry>{std::move(result)};
  }
  if (state->error)
    co_return std::unexpected{*state->error};
  co_return std::optional<dir_entry>{};
}

/** @brief 关闭目录枚举并排空正在执行的读取。 */
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
  expected<void> result;
#if defined(__linux__)
  if (state->native_cursor) {
    auto closed = co_await close_native_directory(state->native_cursor);
    if (!closed)
      result = std::unexpected{closed.error()};
  } else
#endif
    result = co_await blocking::call_cleanup(state->context, [state]() -> expected<void> {
      state->context.unregister_shutdown_cleanup(state->shutdown_registration);
      DIR* native;
      {
        std::lock_guard lock(state->mutex);
        native = std::exchange(state->native, nullptr);
      }
      if (native && ::closedir(native))
        return std::unexpected{make_error(errno)};
      return {};
    });
  if (!result)
    state->close_error = result.error();
  state->close_completion.notify();
  state->context.domain()->release_cleanup_wait();
  co_return result;
}
}  // namespace detail

/**  @brief 按有界批次枚举的异步目录迭代器，无法在目录被外部修改时保证快照语义。 */
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
#if defined(__linux__)
  if (io::supports_native(context, io::detail::operation_kind::OPEN)) {
    auto opened =
        co_await detail::open_native_directory(context, AT_FDCWD, filename.native(), false);
    if (!opened)
      co_return std::unexpected{opened.error()};
    auto state = std::make_shared<detail::directory_state>(context, filename, nullptr);
    state->native_cursor = std::move(*opened);
    detail::register_directory_shutdown(state);
    co_return directory_reader{std::move(state)};
  }
#endif
  co_return co_await blocking::call(
      context, [context, filename = std::move(filename)]() -> expected<directory_reader> {
        auto* native = ::opendir(filename.c_str());
        if (!native)
          return std::unexpected{make_error(errno)};
        std::shared_ptr<detail::directory_state> state;
        try {
          state = std::make_shared<detail::directory_state>(context, filename, native);
        } catch (...) {
          ::closedir(native);
          throw;
        }
        detail::register_directory_shutdown(state);
        return directory_reader{std::move(state)};
      });
}

/** @brief 异步打开目录，返回目录枚举器。 */
inline auto read_dir(fs::path filename) {
  return read_dir(io::io_context::current(), std::move(filename));
}
}  // namespace faio::fs
