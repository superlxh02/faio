#pragma once
/** @file operations.hpp @brief Windows 路径 API、递归目录操作和文件组合接口。 */
#include "faio/detail/fs/windows/directory_cursor.hpp"
#include "faio/detail/fs/windows/file.hpp"
#include <stop_token>

namespace faio::fs {
namespace detail {
/** @brief 统一路径控制句柄；BACKUP_SEMANTICS 同时支持文件与目录。
 * @param follow false 表示操作重解析点本身，防止删除越过链接边界。
 */
inline expected<io::windows::owned_file_handle> open_path_handle(
    const path& filename, DWORD access = FILE_READ_ATTRIBUTES, bool follow = true) {
  auto name = native_path(filename);
  if (!name)
    return std::unexpected{name.error()};
  io::windows::owned_file_handle handle{
      ::CreateFileW(name->c_str(),
                    access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT),
                    nullptr)};
  if (!handle)
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  return handle;
}

/** @brief 已有路径的元数据查询；不跟随模式检查符号链接/挂载点自身。 */
inline auto metadata_path(io::io_context context, path filename, bool follow) {
  return execution::execute(
      std::move(context), [filename = std::move(filename), follow]() -> expected<Metadata> {
        auto handle = open_path_handle(filename, FILE_READ_ATTRIBUTES, follow);
        if (!handle)
          return std::unexpected{handle.error()};
        return metadata_handle(handle->get());
      });
}

inline bool missing(const Error& error) noexcept {
  return error.domain() == error_domain::win32
         && (error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND);
}
}  // namespace detail

inline auto metadata(io::io_context context, path filename) {
  return detail::metadata_path(std::move(context), std::move(filename), true);
}

inline auto symlink_metadata(io::io_context context, path filename) {
  return detail::metadata_path(std::move(context), std::move(filename), false);
}

/** @brief 仅设置 FILE_ATTRIBUTE_READONLY，保留 ACL、其它属性和时间戳。 */
inline auto set_permissions(io::io_context context, path filename, Permissions permissions) {
  return execution::execute(
      std::move(context), [filename = std::move(filename), permissions]() -> expected<void> {
        auto handle =
            detail::open_path_handle(filename, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES);
        if (!handle)
          return std::unexpected{handle.error()};
        return detail::set_handle_permissions(handle->get(), permissions);
      });
}

/** @brief 返回句柄解析后的最终绝对路径，保留 UTF-16 与长路径支持。 */
inline auto canonicalize(io::io_context context, path filename) {
  return execution::execute(
      std::move(context), [filename = std::move(filename)]() -> expected<path> {
        auto handle = detail::open_path_handle(filename);
        if (!handle)
          return std::unexpected{handle.error()};
        DWORD required = ::GetFinalPathNameByHandleW(
            handle->get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!required)
          return std::unexpected{io::windows::make_windows_error(::GetLastError())};
        std::wstring final_path(static_cast<std::size_t>(required) + 1, L'\0');
        for (;;) {
          const DWORD count = ::GetFinalPathNameByHandleW(handle->get(),
                                                          final_path.data(),
                                                          static_cast<DWORD>(final_path.size()),
                                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
          if (!count)
            return std::unexpected{io::windows::make_windows_error(::GetLastError())};
          if (count < final_path.size()) {
            final_path.resize(count);
            return path{std::move(final_path)};
          }
          final_path.resize(static_cast<std::size_t>(count) + 1);
        }
      });
}

/** @brief 同卷重命名并替换已有文件；跨卷返回 OS 错误而不隐式复制。 */
inline auto rename(io::io_context context, path source, path destination) {
  return execution::execute(
      std::move(context),
      [source = std::move(source), destination = std::move(destination)]() -> expected<void> {
        auto from = detail::native_path(source), to = detail::native_path(destination);
        if (!from)
          return std::unexpected{from.error()};
        if (!to)
          return std::unexpected{to.error()};
        if (!::MoveFileExW(from->c_str(), to->c_str(), MOVEFILE_REPLACE_EXISTING))
          return std::unexpected{io::windows::make_windows_error(::GetLastError())};
        return {};
      });
}

/** @brief 删除文件或目录链接自身；普通目录由 remove_dir 专门处理。 */
inline task<expected<void>> remove_file(io::io_context context, path filename) {
  auto result = co_await execution::execute(
      std::move(context), [filename = std::move(filename)]() -> expected<std::int64_t> {
        auto name = detail::native_path(filename);
        if (!name)
          return std::unexpected{name.error()};
        const DWORD attributes = ::GetFileAttributesW(name->c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
          return std::unexpected{io::windows::make_windows_error(::GetLastError())};
        // Windows 目录链接必须用 RemoveDirectory，普通目录不得被 remove_file 删除。
        const bool directory_link =
            (attributes & FILE_ATTRIBUTE_DIRECTORY) && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !directory_link)
          return std::unexpected{make_error(EISDIR)};
        if (!(directory_link ? ::RemoveDirectoryW(name->c_str()) : ::DeleteFileW(name->c_str())))
          return std::unexpected{io::windows::make_windows_error(::GetLastError())};
        return std::int64_t{1};  // 取消仲裁仍保留已经删除一项的副作用。
      });
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}

/** @brief 创建硬链接；原生错误包括跨卷、目标已存在和文件系统不支持。 */
inline auto hard_link(io::io_context context, path source, path destination) {
  return execution::execute(
      std::move(context),
      [source = std::move(source), destination = std::move(destination)]() -> expected<void> {
        auto from = detail::native_path(source), to = detail::native_path(destination);
        if (!from)
          return std::unexpected{from.error()};
        if (!to)
          return std::unexpected{to.error()};
        if (!::CreateHardLinkW(to->c_str(), from->c_str(), nullptr))
          return std::unexpected{io::windows::make_windows_error(::GetLastError())};
        return {};
      });
}

/** @brief 创建文件/目录符号链接，保持相对 target 的原始表示。
 * @details 支持 Developer Mode 的免提权标志；旧系统仅在不支持标志时重试。
 *          本 API 根据现存目标类型选择目录标志，悬空目标按文件链接处理。
 */
inline auto symlink(io::io_context context, path source, path destination) {
  return execution::execute(
      std::move(context),
      [source = std::move(source), destination = std::move(destination)]() -> expected<void> {
        auto to = detail::native_path(destination);
        if (!to)
          return std::unexpected{to.error()};
        if (source.empty() || source.native().find(L'\0') != std::wstring::npos)
          return std::unexpected{make_error(EINVAL)};
        const auto resolved_target =
            source.is_absolute() ? source : destination.parent_path() / source;
        auto from = detail::native_path(resolved_target);
        if (!from)
          return std::unexpected{from.error()};
        const DWORD attributes = ::GetFileAttributesW(from->c_str());
        const DWORD type =
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
                ? SYMBOLIC_LINK_FLAG_DIRECTORY
                : 0;
        const auto target = source.is_absolute() ? *from : source.native();
        if (!::CreateSymbolicLinkW(to->c_str(), target.c_str(), type | 0x2)) {
          const DWORD error = ::GetLastError();
          if (error != ERROR_INVALID_PARAMETER
              || !::CreateSymbolicLinkW(to->c_str(), target.c_str(), type))
            return std::unexpected{io::windows::make_windows_error(
                error == ERROR_INVALID_PARAMETER ? ::GetLastError() : error)};
        }
        return {};
      });
}

/** @brief 创建单层目录；权限参数只影响本次新建对象的只读位。 */
inline auto create_dir(io::io_context context,
                       path filename,
                       Permissions permissions = Permissions{std::filesystem::perms::all}) {
  return execution::execute(
      std::move(context), [filename = std::move(filename), permissions]() -> expected<void> {
        auto name = detail::native_path(filename);
        if (!name)
          return std::unexpected{name.error()};
        if (!::CreateDirectoryW(name->c_str(), nullptr))
          return std::unexpected{io::windows::make_windows_error(::GetLastError())};
        if (permissions.readonly()
            && !::SetFileAttributesW(name->c_str(),
                                     FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY))
          return std::unexpected{io::windows::make_windows_error(::GetLastError(), 1)};
        return {};
      });
}

/** @brief 逐层建立，已有目录仅验证类型，不改写其权限或 ACL。 */
inline task<expected<void>> create_dir_all(io::io_context context,
                                           path filename,
                                           Permissions permissions = Permissions{
                                               std::filesystem::perms::all}) {
  if (filename.empty())
    co_return std::unexpected{make_error(ENOENT)};
  path prefix;
  std::size_t depth = 0;
  for (const auto& component : filename) {
    prefix /= component;
    if (prefix == filename.root_path() || prefix == filename.root_name())
      continue;
    if (++depth > 256 || prefix.native().size() > 32767)
      co_return std::unexpected{make_error(ENAMETOOLONG)};
    auto result = co_await create_dir(context, prefix, permissions);
    if (!result) {
      if (result.error().domain() != error_domain::win32
          || result.error().value() != ERROR_ALREADY_EXISTS)
        co_return std::unexpected{result.error()};
      auto attributes = co_await metadata(context, prefix);
      if (!attributes)
        co_return std::unexpected{attributes.error()};
      if (!attributes->is_dir())
        co_return std::unexpected{make_error(ENOTDIR)};
    }
    co_await this_coro::yield_if_needed();
  }
  co_return expected<void>{};
}

/** @brief 删除空目录或目录链接自身，非空目录保留并返回原生错误。 */
inline auto remove_dir(io::io_context context, path filename) {
  return execution::execute(
      std::move(context), [filename = std::move(filename)]() -> expected<void> {
        auto name = detail::native_path(filename);
        if (!name)
          return std::unexpected{name.error()};
        if (!::RemoveDirectoryW(name->c_str()))
          return std::unexpected{io::windows::make_windows_error(::GetLastError())};
        return {};
      });
}

/** @brief 深度/内存/协作批次的资源上限；状态包括活跃目录的原生枚举缓存。 */
struct recursive_remove_options {
  std::size_t max_depth{256};
  std::size_t max_state_bytes{1024 * 1024};
  std::size_t batch_size{64};
};

namespace detail {
/** @brief 句柄相对后序 DFS；所有 reparse point 仅删除自身，绝不进入其目标。 */
struct remove_tree_state {
  struct frame {
    std::shared_ptr<native_directory_cursor> cursor;
    std::size_t bytes;
  };

  io::io_context context;
  path root;
  recursive_remove_options options;
  std::vector<frame> stack;
  std::size_t state_bytes{};
  bool initialized{}, done{};

  remove_tree_state(io::io_context owner, path name, recursive_remove_options limits)
      : context(std::move(owner)),
        root(std::move(name)),
        options(limits),
        state_bytes(root.native().size() * sizeof(wchar_t)) {}

  /** @brief 一个服务任务最多处理 batch_size 步；错误附带本批真实删除条数。 */
  expected<std::uint64_t> advance(std::stop_token stop) {
    std::uint64_t removed = 0;
    auto failure = [&](const Error& error) -> expected<std::uint64_t> {
      return std::unexpected{Error{error.value(), removed + error.progress(), error.domain()}};
    };
    for (std::size_t step = 0; step < options.batch_size && !done; ++step) {
      if (stop.stop_requested() || context.stopped())
        return failure(make_error(ECANCELED));
      if (!initialized) {
        initialized = true;
        auto handle =
            open_path_handle(root, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, false);
        if (!handle) {
          if (missing(handle.error())) {
            done = true;
            break;
          }
          return failure(handle.error());
        }
        auto attributes = metadata_handle(handle->get());
        if (!attributes)
          return failure(attributes.error());
        FILE_ATTRIBUTE_TAG_INFO root_tag{};
        if (!::GetFileInformationByHandleEx(
                handle->get(), FileAttributeTagInfo, &root_tag, sizeof(root_tag)))
          return failure(io::windows::make_windows_error(::GetLastError()));
        if (!attributes->is_dir() || (root_tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
          auto result = delete_handle(handle->get());
          if (!result)
            return failure(result.error());
          ++removed;
          done = true;
          break;
        }
        if (sizeof(native_directory_cursor) + sizeof(frame) > options.max_state_bytes - state_bytes)
          return failure(make_error(ENAMETOOLONG));
        auto cursor = std::make_shared<native_directory_cursor>(context, handle->get());
        handle->release();
        const auto bytes = sizeof(native_directory_cursor) + sizeof(frame);
        stack.push_back({std::move(cursor), bytes});
        state_bytes += bytes;
        continue;
      }
      auto current = stack.back().cursor;  // 共享租约不借用可能被 push_back 移动的 frame。
      auto entry = current->take();
      if (!entry)
        return failure(entry.error());
      if (!*entry && !current->eof) {
        auto filled = current->fill();
        if (!filled)
          return failure(filled.error());
        continue;
      }
      if (!*entry) {
        auto result = delete_handle(current->handle.get());  // 子项全删后标记当前目录。
        if (!result)
          return failure(result.error());
        state_bytes -= stack.back().bytes;
        stack.pop_back();
        ::CloseHandle(current->release());  // 同一服务线程及时完成删除，无 worker 阻塞。
        ++removed;
        if (stack.empty())
          done = true;
        continue;
      }
      const auto& item = **entry;
      auto child = open_relative(
          current->handle.get(), item.name, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
      if (!child) {
        if (missing(child.error()))
          continue;  // 外部已删除的条目不计入本调用进度。
        return failure(child.error());
      }
      const auto attributes = metadata_handle(child->get());
      if (!attributes)
        return failure(attributes.error());
      // 所有重解析点均不遍历，包括云文件/其它 tag，安全性不依赖枚举提示。
      FILE_ATTRIBUTE_TAG_INFO tag{};
      if (!::GetFileInformationByHandleEx(child->get(), FileAttributeTagInfo, &tag, sizeof(tag)))
        return failure(io::windows::make_windows_error(::GetLastError()));
      if (!attributes->is_dir() || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        auto result = delete_handle(child->get());
        if (!result)
          return failure(result.error());
        ++removed;
        continue;
      }
      if (stack.size() >= options.max_depth)
        return failure(make_error(ELOOP));
      constexpr std::size_t bytes = sizeof(native_directory_cursor) + sizeof(frame);
      if (bytes > options.max_state_bytes - state_bytes)
        return failure(make_error(ENAMETOOLONG));
      auto cursor = std::make_shared<native_directory_cursor>(context, child->get());
      child->release();
      stack.push_back({std::move(cursor), bytes});
      state_bytes += bytes;
    }
    return removed;
  }
};
}  // namespace detail

/** @brief 有界递归删除；取消/异常会通过各游标的保留清理通道关闭剩余 HANDLE。 */
inline task<expected<std::uint64_t>> remove_dir_all(io::io_context context,
                                                    path filename,
                                                    recursive_remove_options options = {}) {
  if (!options.max_depth || !options.batch_size || options.batch_size > 4096
      || filename.native().size() > options.max_state_bytes / sizeof(wchar_t))
    co_return std::unexpected{make_error(EINVAL)};
  auto state = std::make_shared<detail::remove_tree_state>(context, std::move(filename), options);
  const auto stop = co_await this_coro::stop_token();
  std::uint64_t total = 0;
  try {
    while (!state->done) {
      auto count =
          co_await execution::execute(context, [state, stop] { return state->advance(stop); });
      if (!count)
        co_return std::unexpected{
            Error{count.error().value(), total + count.error().progress(), count.error().domain()}};
      total += *count;
      co_await this_coro::yield_if_needed();
    }
  } catch (const operation_cancelled&) {
    co_return std::unexpected{Error{ECANCELED, total}};  // 协作 yield 取消也保留之前所有删除进度。
  }
  co_return total;
}

/** @brief 有界块复制；每块让出协作预算，并保留失败前写入的字节进度。 */
inline task<expected<std::uint64_t>> copy(io::io_context context, path source, path destination) {
  auto input = co_await File::open(context, std::move(source));
  if (!input)
    co_return std::unexpected{input.error()};
  auto source_metadata = co_await input->metadata();
  if (!source_metadata)
    co_return std::unexpected{source_metadata.error()};
  if (!source_metadata->is_file())
    co_return std::unexpected{make_error(EINVAL)};
  // 先打开而不截断，使用持有fd的inode身份识别同路径、硬链接和符号链接自复制。
  auto output =
      co_await File::open(context, std::move(destination), OpenOptions{}.write().create());
  if (!output)
    co_return std::unexpected{output.error()};
  auto destination_metadata = co_await output->metadata();
  if (!destination_metadata)
    co_return std::unexpected{destination_metadata.error()};
  if (source_metadata->same_file(*destination_metadata))
    co_return std::unexpected{make_error(EINVAL)};
  auto truncated = co_await output->set_len(0);
  if (!truncated)
    co_return std::unexpected{truncated.error()};
  auto transferred = co_await io::copy(*input, *output);
  if (!transferred)
    co_return std::unexpected{transferred.error()};
  auto permissions = co_await output->set_permissions(source_metadata->permissions());
  if (!permissions)
    co_return std::unexpected{
        Error{permissions.error().value(), *transferred, permissions.error().domain()}};
  auto closed = co_await output->close();
  if (!closed)
    co_return std::unexpected{Error{closed.error().value(), *transferred, closed.error().domain()}};
  auto input_closed = co_await input->close();
  if (!input_closed)
    co_return std::unexpected{
        Error{input_closed.error().value(), *transferred, input_closed.error().domain()}};
  co_return transferred;
}

/** @brief 读完整文件；max_bytes 限制应用内存，不在一次 job 内分配任意大小。 */
inline task<expected<std::vector<char>>> read(io::io_context context,
                                              path filename,
                                              std::size_t max_bytes = 256 * 1024 * 1024) {
  auto input = co_await File::open(std::move(context), std::move(filename));
  if (!input)
    co_return std::unexpected{input.error()};
  std::vector<char> result;
  auto count = co_await io::read_to_end(*input, result, max_bytes);
  if (!count)
    co_return std::unexpected{count.error()};
  // 达到限制时探测一个字节，避免把截断的文件冒充完整结果。
  if (*count == max_bytes) {
    std::array<char, 1> probe;
    auto tail = co_await input->read(probe);
    if (!tail)
      co_return std::unexpected{tail.error()};
    if (*tail)
      co_return std::unexpected{Error{EFBIG, *count}};
  }
  co_return result;
}

inline task<expected<std::string>> read_to_string(io::io_context context,
                                                  path filename,
                                                  std::size_t max_bytes = 256 * 1024 * 1024) {
  auto contents = co_await read(std::move(context), std::move(filename), max_bytes);
  if (!contents)
    co_return std::unexpected{contents.error()};
  co_return std::string(contents->begin(), contents->end());
}

/** @brief 借用写入，等待关闭后才返回；输入必须覆盖整个等待期间。 */
inline task<expected<void>> write(io::io_context context,
                                  path filename,
                                  std::span<const char> contents) {
  auto output = co_await File::create(std::move(context), std::move(filename));
  if (!output)
    co_return std::unexpected{output.error()};
  auto written = co_await output->write_all(contents);
  if (!written)
    co_return std::unexpected{written.error()};
  co_return co_await output->close();
}

inline task<expected<void>> write(io::io_context context, path filename, io::io_buffer contents) {
  co_return co_await write(std::move(context), std::move(filename), contents.bytes());
}

// 默认 context 只在创建请求时读取；操作、路径与析构不依赖未来线程的 TLS。
inline auto metadata(path name) {
  return metadata(io::io_context::current(), std::move(name));
}

inline auto symlink_metadata(path name) {
  return symlink_metadata(io::io_context::current(), std::move(name));
}

inline auto set_permissions(path name, Permissions value) {
  return set_permissions(io::io_context::current(), std::move(name), value);
}

inline auto canonicalize(path name) {
  return canonicalize(io::io_context::current(), std::move(name));
}

inline auto rename(path from, path to) {
  return rename(io::io_context::current(), std::move(from), std::move(to));
}

inline auto remove_file(path name) {
  return remove_file(io::io_context::current(), std::move(name));
}

inline auto hard_link(path from, path to) {
  return hard_link(io::io_context::current(), std::move(from), std::move(to));
}

inline auto symlink(path from, path to) {
  return symlink(io::io_context::current(), std::move(from), std::move(to));
}

inline auto create_dir(path name, Permissions value = Permissions{std::filesystem::perms::all}) {
  return create_dir(io::io_context::current(), std::move(name), value);
}

inline auto create_dir_all(path name) {
  return create_dir_all(io::io_context::current(), std::move(name));
}

inline auto remove_dir(path name) {
  return remove_dir(io::io_context::current(), std::move(name));
}

inline auto remove_dir_all(path name, recursive_remove_options options = {}) {
  return remove_dir_all(io::io_context::current(), std::move(name), options);
}

inline auto copy(path from, path to) {
  return copy(io::io_context::current(), std::move(from), std::move(to));
}

inline auto read(path name, std::size_t limit = 256 * 1024 * 1024) {
  return read(io::io_context::current(), std::move(name), limit);
}

inline auto read_to_string(path name, std::size_t limit = 256 * 1024 * 1024) {
  return read_to_string(io::io_context::current(), std::move(name), limit);
}

inline auto write(path name, std::span<const char> contents) {
  return write(io::io_context::current(), std::move(name), contents);
}

inline auto write(path name, io::io_buffer contents) {
  return write(io::io_context::current(), std::move(name), std::move(contents));
}

inline auto write(path name, std::string_view contents) {
  return write(io::io_context::current(),
               std::move(name),
               std::span<const char>{contents.data(), contents.size()});
}

/** @brief 可复用的目录创建配置，权限只影响本次新建目录。 */
class DirBuilder {
 public:
  DirBuilder& recursive(bool enabled = true) noexcept {
    recursive_ = enabled;
    return *this;
  }

  DirBuilder& permissions(Permissions value) noexcept {
    permissions_ = value;
    return *this;
  }

  auto create(io::io_context context, path name) const {
    // 配置在构造请求时复制；临时DirBuilder的生命周期不必覆盖co_await。
    return create_owned(std::move(context), std::move(name), recursive_, permissions_);
  }

  auto create(path name) const { return create(io::io_context::current(), std::move(name)); }

 private:
  /** @brief 两种 awaiter 经一个 task 统一类型；所有配置按值复制，不借用 this。 */
  static task<expected<void>> create_owned(io::io_context context,
                                           path name,
                                           bool recursive,
                                           Permissions permissions) {
    if (recursive)
      co_return co_await create_dir_all(std::move(context), std::move(name), permissions);
    co_return co_await create_dir(std::move(context), std::move(name), permissions);
  }

  bool recursive_{};
  Permissions permissions_{std::filesystem::perms::all};
};
}  // namespace faio::fs
