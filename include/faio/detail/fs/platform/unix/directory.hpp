/**
 * @file faio/detail/fs/platform/unix/directory.hpp
 * @brief 文件与目录异步接口：目录项与异步枚举。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include "faio/detail/fs/operations.hpp"
#include <deque>
#include <dirent.h>

namespace faio::fs {
/** @brief 拥有目录条目路径；不借用 readdir 临时内存，metadata
 * 按需另行异步获取。 */
class DirEntry {
 public:
  DirEntry(io::io_context context, fs::path filename, unsigned char type)
      : context_(std::move(context)), path_(std::move(filename)), type_(type) {}

  [[nodiscard]] fs::path path() const { return path_; }

  [[nodiscard]] fs::path file_name() const { return path_.filename(); }

  auto metadata() const { return fs::metadata(context_, path_); }

  /** @brief d_type 已知时立即完成；未知时异步 lstat，不预先 stat 整个目录。 */
  auto file_type() const { return determine_type(context_, path_, type_); }

 private:
  /** @brief 创建task时拥有路径/context/type，临时DirEntry销毁后仍可安全等待。
   */
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
  io::io_context context_;  ///< 持有所属 IO 上下文租约，用于资源归属、服务选择与安全排空。
  fs::path path_;  ///< 拥有的文件系统路径，覆盖异步提交与执行期间。
  unsigned char type_;  ///< 目录项的原生类型提示，完整元数据查询仍以系统结果为准。
};

namespace detail {
/** @brief 独占目录游标；最多缓存64条或64KiB名称，取消后已枚举条目仍可领取。 */
struct directory_state {
  std::uint64_t shutdown_registration{};  ///< 向 IO 域登记的停机清理编号，正常关闭时撤销。
  io::io_context context;  ///< 持有所属 IO 上下文租约，用于资源归属、服务选择与安全排空。
  fs::path path;  ///< 拥有的文件系统路径，覆盖异步提交与执行期间。
  DIR* native{};  ///< 本次遍历拥有的原生目录枚举句柄。
#if defined(__linux__)
  std::shared_ptr<native_directory_cursor> native_cursor;  ///< 共享持有 Linux 原生目录游标，异步批次与关闭期间保持枚举状态稳定。
#endif
  std::mutex mutex;  ///< 串行保护所属对象的共享状态；调度与用户回调应遵循该对象的锁外调用协议。
  bool busy{},  ///< 是否已有目录枚举或相关操作正在推进。
      closing{},  ///< 资源已经进入关闭阶段，拒绝新的业务操作。
      eof{},  ///< 目录或输入是否已到末尾，后续读取据此立即返回。
      shutdown_pending{},  ///< 域停机是否已登记本资源的关闭请求。
      shutdown_close_started{};  ///< 停机关闭操作是否已真正启动，防止重复关闭。
  std::optional<Error> error;  ///< 保存的操作错误，供完成或重复查询返回。
  std::optional<Error> close_error;  ///< 关闭过程保存的错误，供重复关闭等待者取得同一结果。
  ::faio::detail::completion_event close_completion;  ///< 带记忆的关闭完成事件，所有关闭等待者共享同一完成通知。
  std::deque<DirEntry> entries;  ///< 已解析但尚未交付给调用者的目录项队列。
  std::shared_ptr<::faio::detail::completion_event> pending;  ///< 当前在途目录批次的共享完成事件。

  directory_state(io::io_context owner, fs::path name, DIR* directory)
      : context(std::move(owner)), path(std::move(name)), native(directory) {}

  ~directory_state() {
    context.unregister_shutdown_cleanup(shutdown_registration);
    auto* directory = std::exchange(native, nullptr);
    if (directory)
      context.defer_cleanup([directory] { ::closedir(directory); });
  }
};

/** @brief 最后一个枚举租约结束后发起关闭；没有线程等待busy条件变量。 */
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

/** @brief 注册弱目录清理；实际关闭交由最后一个lease或当前空闲状态触发。 */
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

/** @brief 请求租约保证 close 等待批量 syscall 和缓存更新全部结束。 */
struct directory_lease {
  std::shared_ptr<directory_state> state;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
  std::shared_ptr<::faio::detail::completion_event> completion;  ///< 带记忆的完成事件，通知后晚到等待者也可直接继续。

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
        /// @brief 仅解析已拥有的缓存。
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
          /// @brief 唯一缺少原生opcode的getdents64在服务线程填充4KiB缓存。
          auto filled =
              co_await execution::execute(state->context, [cursor] { return cursor->fill(); });
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
      auto batch = co_await execution::execute(state->context, [state]() -> expected<void> {
        std::size_t count = 0, bytes = 0;
        while (count < 64 && bytes < 65536) {
          /// @brief readdir 的空指针既可能 EOF，也可能发生错误。
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
          /// @brief 复制完整名称和类型，下一次 readdir 可以复用原生内存。
          state->entries.emplace_back(state->context, state->path / fs::path{name}, entry->d_type);
          ++count;
          bytes += name.size();
        }
        return {};
      });
      /// @brief 取消只取消本次领取；job 更新的拥有型缓存留在 directory_state 中。
      if (!batch)
        co_return std::unexpected{batch.error()};
    }
  }
  if (!state->entries.empty()) {
    auto result = std::move(state->entries.front());
    state->entries.pop_front();
    co_return std::optional<DirEntry>{std::move(result)};
  }
  if (state->error)
    co_return std::unexpected{*state->error};
  co_return std::optional<DirEntry>{};
}

inline task<expected<void>> close_directory(std::shared_ptr<directory_state> state) {
  if (!state)
    co_return expected<void>{};
  std::shared_ptr<::faio::detail::completion_event> pending;
  bool owner = false;
  {
    std::lock_guard lock(state->mutex);
    if (!state->closing) {
      owner = true;
      /// @brief 先拒绝新next_entry，再等已有枚举job完全结束。
      state->closing = true;
      state->context.domain()->retain_cleanup_wait();
      if (state->busy)
        pending = state->pending;
    }
  }
  if (!owner) {
    /// @brief 并发close也等同一真实CLOSE完成。
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
    result = co_await execution::execute_cleanup(state->context, [state]() -> expected<void> {
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
}

/** @brief 有界批量异步目录迭代器；外部目录变化不提供快照语义。 */
class ReadDir {
 public:
  explicit ReadDir(std::shared_ptr<detail::directory_state> state) : state_(std::move(state)) {}

  ReadDir(ReadDir&&) noexcept = default;

  ReadDir& operator=(ReadDir&&) noexcept = default;

  ReadDir(const ReadDir&) = delete;

  ReadDir& operator=(const ReadDir&) = delete;

  auto next_entry() { return detail::next_entry(state_); }

  /// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
  auto close() { return detail::close_directory(state_); }

 private:
  std::shared_ptr<detail::directory_state> state_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
};

inline task<expected<ReadDir>> read_dir(io::io_context context, fs::path filename) {
#if defined(__linux__)
  if (io::supports_native(context, io::detail::operation_kind::open)) {
    auto opened =
        co_await detail::open_native_directory(context, AT_FDCWD, filename.native(), false);
    if (!opened)
      co_return std::unexpected{opened.error()};
    auto state = std::make_shared<detail::directory_state>(context, filename, nullptr);
    state->native_cursor = std::move(*opened);
    detail::register_directory_shutdown(state);
    co_return ReadDir{std::move(state)};
  }
#endif
  co_return co_await execution::execute(
      context, [context, filename = std::move(filename)]() -> expected<ReadDir> {
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
        return ReadDir{std::move(state)};
      });
}

inline auto read_dir(fs::path filename) {
  return read_dir(io::io_context::current(), std::move(filename));
}
}
