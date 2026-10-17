#pragma once
/**
 * @file faio/detail/fs/platform/windows/directory.hpp
 * @brief Windows 有界目录迭代与取消/关闭租约。
 */
#include "faio/detail/fs/platform/windows/operations.hpp"
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

  io::io_context context_;  ///< 持有所属 IO 上下文租约，用于资源归属、服务选择与安全排空。
  fs::path path_;  ///< 拥有的文件系统路径，覆盖异步提交与执行期间。
  DWORD attributes_{};  ///< Windows 原生文件属性，用于分类与权限映射。
};

namespace detail {
/** @brief 一个目录枚举 lane，最多缓存 64 项或 64 KiB 名称。 */
struct directory_state {
  std::uint64_t shutdown_registration{};  ///< 向 IO 域登记的停机清理编号，正常关闭时撤销。
  io::io_context context;  ///< 持有所属 IO 上下文租约，用于资源归属、服务选择与安全排空。
  fs::path path;  ///< 拥有的文件系统路径，覆盖异步提交与执行期间。
  std::shared_ptr<native_directory_cursor> cursor;  ///< 共享文件位置或目录枚举游标，按具体平台实现推进。
  std::mutex mutex;  ///< admission 和 close 短锁，原生调用从不占有此锁。
  bool busy{},  ///< 是否已有目录枚举或相关操作正在推进。
      closing{},  ///< 资源已经进入关闭阶段，拒绝新的业务操作。
      eof{},  ///< 目录或输入是否已到末尾，后续读取据此立即返回。
      shutdown_pending{},  ///< 域停机是否已登记本资源的关闭请求。
      shutdown_close_started{};  ///< 停机关闭操作是否已真正启动，防止重复关闭。
  std::optional<Error> error,  ///< 保存的操作错误，供完成或重复查询返回。
      close_error;  ///< 关闭过程保存的错误，供重复关闭等待者取得同一结果。
  ::faio::detail::completion_event close_completion;  ///< 带记忆的关闭完成事件，所有关闭等待者共享同一完成通知。
  std::deque<DirEntry> entries;  ///< 已解析但尚未交付给调用者的目录项队列。
  std::shared_ptr<::faio::detail::completion_event> pending;  ///< 当前在途目录批次的共享完成事件。

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
        /// @brief 资源预算按真实名称存储字节计算。
        bytes += item.name.size() * sizeof(wchar_t);
        state->entries.emplace_back(
            state->context, state->path / fs::path{std::move(item.name)}, item.attributes);
        ++count;
      }
      return {};
    });
    /// @brief 取消后已枚举缓存仍保留可领取。
    if (!result)
      co_return std::unexpected{result.error()};
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
}

/** @brief 移动型、有界异步目录迭代器；不提供目录内容快照语义。 */
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
}
