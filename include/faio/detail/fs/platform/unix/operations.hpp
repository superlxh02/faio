/**
 * @file faio/detail/fs/platform/unix/operations.hpp
 * @brief 操作工厂与组合接口。
 */
#pragma once
#include "faio/detail/fs/provider/directory_cursor.hpp"
#include "faio/detail/io/stream/algorithms.hpp"
#include "faio/detail/fs/core/remove_options.hpp"
#include "faio/detail/fs/file.hpp"
#if defined(__linux__)
#include "faio/detail/fs/platform/linux/remove_tree.hpp"
#endif
#include <dirent.h>
#include <filesystem>
#include <string>
#include <vector>

namespace faio::fs {
namespace detail {
/**  @brief 查询路径属性时直接提交 STATX；仅在原生操作码不可用时使用 stat/lstat 服务。 */
inline task<expected<Metadata>> metadata_path(io::io_context context, path filename, bool follow) {
#if defined(__linux__)
  if (io::supports_native(context, io::detail::operation_kind::statx))
    co_return co_await native_metadata_awaiter{
        std::move(context), AT_FDCWD, filename.native(), follow ? 0 : AT_SYMLINK_NOFOLLOW};
#endif
  co_return co_await blocking::call(
      std::move(context), [filename = std::move(filename), follow]() -> expected<Metadata> {
        struct stat value{};
        if (follow ? ::stat(filename.c_str(), &value) : ::lstat(filename.c_str(), &value))
          return std::unexpected{make_error(errno)};
        return Metadata{value};
      });
}

/**  @brief 创建相对于 fd 的请求，独立持有两条路径，不引用临时参数。 */
template <io::detail::operation_kind Kind>
struct path_operation {
  path source;      /**< 文件系统操作的源路径副本。 */
  path destination; /**< 目标文件系统路径的副本，在异步操作执行期间保持有效。 */
  int argument{};   /**< 由具体操作解释的首个原生整数参数。 */
  int flags{};      /**< 本次原生操作的标志位，按对应系统调用语义解释。 */

  /** @brief 返回当前文件请求的操作类型。 */
  static constexpr io::detail::operation_kind kind() noexcept { return Kind; }

  /** @brief 将紧凑请求展开为完整请求，固定地址、移交资源租约并初始化低频字段。 */
  io::detail::io_request into_request() && {
    io::detail::io_request request;
    request.kind = Kind;
    request.fd = AT_FDCWD;
    request.path = source.native();
    request.flags = flags;
    if constexpr (Kind == io::detail::operation_kind::renameat
                  || Kind == io::detail::operation_kind::linkat
                  || Kind == io::detail::operation_kind::symlinkat)
      request.path2 = destination.native();
    if constexpr (Kind == io::detail::operation_kind::renameat
                  || Kind == io::detail::operation_kind::linkat)
      request.argument2 = AT_FDCWD;
    if constexpr (Kind == io::detail::operation_kind::mkdirat)
      request.argument = argument;
    return request;
  }

  /** @brief 执行当前文件请求的同步回退调用。 */
  expected<void> operator()() const noexcept {
    int result;
    if constexpr (Kind == io::detail::operation_kind::renameat)
      result = ::rename(source.c_str(), destination.c_str());
    else if constexpr (Kind == io::detail::operation_kind::linkat)
      result = ::link(source.c_str(), destination.c_str());
    else if constexpr (Kind == io::detail::operation_kind::symlinkat)
      result = ::symlink(source.c_str(), destination.c_str());
    else if constexpr (Kind == io::detail::operation_kind::mkdirat)
      result = ::mkdir(source.c_str(), static_cast<mode_t>(argument));
    else {
      static_assert(Kind == io::detail::operation_kind::unlinkat);
      result = flags & AT_REMOVEDIR ? ::rmdir(source.c_str()) : ::unlink(source.c_str());
    }
    if (result)
      return std::unexpected{make_error(errno)};
    return {};
  }
};
}  // namespace detail

/**  @brief 跟随符号链接取得目标属性；uring 使用原生 STATX。 */
inline auto metadata(io::io_context context, path filename) {
  return detail::metadata_path(std::move(context), std::move(filename), true);
}

/**  @brief 取得链接本身的属性，保留 POSIX 路径中非 UTF-8 的字节。 */
inline auto symlink_metadata(io::io_context context, path filename) {
  return detail::metadata_path(std::move(context), std::move(filename), false);
}

/** @brief 设置目标文件的权限。 */
inline auto set_permissions(io::io_context context, path filename, Permissions permissions) {
  return blocking::call(std::move(context),
                        [filename = std::move(filename), permissions]() -> expected<void> {
                          if (::chmod(filename.c_str(), static_cast<mode_t>(permissions.bits())))
                            return std::unexpected{make_error(errno)};
                          return {};
                        });
}

/** @brief 解析符号链接并返回规范化绝对路径。 */
inline auto canonicalize(io::io_context context, path filename) {
  return blocking::call(std::move(context), [filename = std::move(filename)]() -> expected<path> {
    std::error_code error;
    auto result = std::filesystem::canonical(filename, error);
    if (error)
      return std::unexpected{make_error(error.value())};
    return result;
  });
}

/** @brief 将路径重命名或移动到目标位置。 */
inline auto rename(io::io_context context, path source, path destination) {
  return detail::void_file_request(std::move(context),
                                   detail::path_operation<io::detail::operation_kind::renameat>{
                                       std::move(source), std::move(destination)});
}

/** @brief 删除一个文件或符号链接路径。 */
inline auto remove_file(io::io_context context, path filename) {
  return detail::void_file_request(
      std::move(context),
      detail::path_operation<io::detail::operation_kind::unlinkat>{std::move(filename), {}});
}

/** @brief 为已有文件创建硬链接。 */
inline auto hard_link(io::io_context context, path source, path destination) {
  return detail::void_file_request(std::move(context),
                                   detail::path_operation<io::detail::operation_kind::linkat>{
                                       std::move(source), std::move(destination)});
}

/** @brief 创建指向指定目标的符号链接。 */
inline auto symlink(io::io_context context, path source, path destination) {
  return detail::void_file_request(std::move(context),
                                   detail::path_operation<io::detail::operation_kind::symlinkat>{
                                       std::move(source), std::move(destination)});
}

/** @brief 创建单个目录。 */
inline auto create_dir(io::io_context context,
                       path filename,
                       Permissions permissions = Permissions{std::filesystem::perms::all}) {
  return detail::void_file_request(
      std::move(context),
      detail::path_operation<io::detail::operation_kind::mkdirat>{
          std::move(filename), {}, static_cast<int>(permissions.bits())});
}

/**  @brief 使用原生 MKDIRAT 逐级创建目录；通过 STATX 验证已有目录，不修改其权限。 */
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
    if (prefix == filename.root_path())
      continue;
    if (++depth > 256 || prefix.native().size() > 1024 * 1024)
      co_return std::unexpected{make_error(ENAMETOOLONG)};
    auto created = co_await create_dir(context, prefix, permissions);
    if (!created) {
      if (created.error().value() != EEXIST)
        co_return created;
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

/** @brief 删除一个空目录。 */
inline auto remove_dir(io::io_context context, path filename) {
  return detail::void_file_request(std::move(context),
                                   detail::path_operation<io::detail::operation_kind::unlinkat>{
                                       std::move(filename), {}, 0, AT_REMOVEDIR});
}

namespace detail {
/**
 *  @brief
 * 相对于 fd 进行深度优先遍历，通过 O_NOFOLLOW 和 AT_SYMLINK_NOFOLLOW 避免进入目录链接的目标。
 */
struct remove_tree_state {
  /** @brief 递归目录遍历中持有单级目录及处理进度的栈帧。 */
  struct frame {
    DIR* directory;   /**< 递归遍历时保持打开的原生目录游标。 */
    int parent;       /**< 父目录保持打开，子目录删除不依赖会变化的完整路径。 */
    std::string name; /**< 持有目录项名称，避免借用会被下一批枚举覆盖的存储。 */
  };

  io::io_context context; /**< 持有所属 I/O 上下文租约，用于确定资源归属、选择服务并安全排空。 */
  path root;              /**< 递归文件操作的根路径副本。 */
  recursive_remove_options options; /**< 当前文件操作的选项副本。 */
  std::vector<frame> stack;         /**< 递归删除的显式遍历栈，避免递归调用栈随目录深度增长。 */
  std::size_t state_bytes{};        /**< 当前遍历状态占用的字节数，用于检查内存预算。 */
  bool initialized{}, done{};       /**< 操作是否已结束，后续推进据此直接返回。 */

  /** @brief 保存递归删除的路径、资源预算和 I/O 上下文。 */
  remove_tree_state(io::io_context owner, path filename, recursive_remove_options limits)
      : context(std::move(owner)),
        root(std::move(filename)),
        options(limits),
        state_bytes(root.native().size()) {}

  /** @brief 释放递归删除持有的目录句柄。 */
  ~remove_tree_state() {
    if (stack.empty())
      return;
    context.defer_cleanup([remaining = std::move(stack)] {
      for (auto it = remaining.rbegin(); it != remaining.rend(); ++it)
        ::closedir(it->directory);
    });
  }

  /**  @brief 每步开始前检查停止状态，返回本批已删除的条目数；错误进度也以条目数表示。 */
  expected<std::uint64_t> advance(std::stop_token parent_stop) {
    std::uint64_t removed = 0;
    auto failure = [&](int error) -> expected<std::uint64_t> {
      return std::unexpected{Error{error, static_cast<std::size_t>(removed)}};
    };
    for (std::size_t step = 0; step < options.batch_size && !done; ++step) {
      if (parent_stop.stop_requested() || context.stopped())
        return failure(ECANCELED);
      if (!initialized) {
        initialized = true;
        struct stat attributes;
        if (::lstat(root.c_str(), &attributes)) {
          if (errno == ENOENT) {
            done = true;
            break;
          }
          return failure(errno);
        }
        if (!S_ISDIR(attributes.st_mode)) {
          if (::unlink(root.c_str()))
            return failure(errno);
          ++removed;
          done = true;
          break;
        }
        const int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
          return failure(errno);
        auto* directory = ::fdopendir(fd);
        if (!directory) {
          const int error = errno;
          ::close(fd);
          return failure(error);
        }
        try {
          stack.push_back({directory, -1, {}});
        } catch (...) {
          ::closedir(directory);
          throw;
        }
        continue;
      }
      auto& current = stack.back();
      errno = 0;
      auto* entry = ::readdir(current.directory);
      if (!entry) {
        if (errno)
          return failure(errno);
        const int parent = current.parent;
        auto name = std::move(current.name);
        auto* directory = current.directory;
        stack.pop_back();
        state_bytes -= name.size();
        if (::closedir(directory))
          return failure(errno);
        const int result =
            parent < 0 ? ::rmdir(root.c_str()) : ::unlinkat(parent, name.c_str(), AT_REMOVEDIR);
        if (result)
          return failure(errno);
        ++removed;
        if (stack.empty())
          done = true;
        continue;
      }
      const std::string_view name{entry->d_name};
      if (name == "." || name == "..")
        continue;
      const int parent = ::dirfd(current.directory);
      struct stat attributes;
      if (::fstatat(parent, entry->d_name, &attributes, AT_SYMLINK_NOFOLLOW))
        return failure(errno);
      if (!S_ISDIR(attributes.st_mode)) {
        if (::unlinkat(parent, entry->d_name, 0))
          return failure(errno);
        ++removed;
        continue;
      }
      if (stack.size() >= options.max_depth)
        return failure(ELOOP);
      if (name.size() > options.max_state_bytes - state_bytes)
        return failure(ENAMETOOLONG);
      std::string owned_name{name};
      const int fd =
          ::openat(parent, owned_name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0)
        return failure(errno);
      auto* directory = ::fdopendir(fd);
      if (!directory) {
        const int error = errno;
        ::close(fd);
        return failure(error);
      }
      try {
        stack.push_back({directory, parent, std::move(owned_name)});
      } catch (...) {
        ::closedir(directory);
        throw;
      }
      state_bytes += name.size();
    }
    return removed;
  }
};

}  // namespace detail

/**
 * @brief 有界分批递归删除，不跟随目录符号链接；失败返回累计已删除条目数。
 * @details
 * 删除操作不具有原子性。每步检查取消状态，已开始的短批次排空后才交还请求状态。
 */
inline task<expected<std::uint64_t>> remove_dir_all(io::io_context context,
                                                    path filename,
                                                    recursive_remove_options options = {}) {
  if (!options.max_depth || !options.batch_size || options.batch_size > 4096
      || filename.native().size() > options.max_state_bytes)
    co_return std::unexpected{make_error(EINVAL)};
#if defined(__linux__)
  if (io::supports_native(context, io::detail::operation_kind::open)
      && io::supports_native(context, io::detail::operation_kind::statx))
    co_return co_await detail::remove_native_tree(context, std::move(filename), options);
#endif
  auto state = std::make_shared<detail::remove_tree_state>(context, std::move(filename), options);
  auto stop = co_await this_coro::stop_token();
  std::uint64_t total = 0;
  while (!state->done) {
    auto batch = co_await blocking::call(context, [state, stop] { return state->advance(stop); });
    if (!batch)
      co_return std::unexpected{
          batch.error().with_progress(static_cast<std::size_t>(total) + batch.error().progress())};
    total += *batch;
    co_await this_coro::yield_if_needed();
  }
  co_return total;
}

/**  @brief 按有界数据块复制，每块交还协作预算；失败时保留已写入的字节数。 */
inline task<expected<std::uint64_t>> copy(io::io_context context, path source, path destination) {
  auto input = co_await File::open(context, std::move(source));
  if (!input)
    co_return std::unexpected{input.error()};
  auto source_metadata = co_await input->metadata();
  if (!source_metadata)
    co_return std::unexpected{source_metadata.error()};
  if (!source_metadata->is_file())
    co_return std::unexpected{make_error(EINVAL)};
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
    co_return std::unexpected{permissions.error().with_progress(*transferred)};
  auto closed = co_await output->close();
  if (!closed)
    co_return std::unexpected{closed.error().with_progress(*transferred)};
  auto input_closed = co_await input->close();
  if (!input_closed)
    co_return std::unexpected{input_closed.error().with_progress(*transferred)};
  co_return transferred;
}

/** @brief 将完整文件读入自有缓冲区，并检查字节上限。 */
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

/** @brief 读取字节并追加到字符串，按已完成的字节进度传播错误。 */
inline task<expected<std::string>> read_to_string(io::io_context context,
                                                  path filename,
                                                  std::size_t max_bytes = 256 * 1024 * 1024) {
  auto contents = co_await read(std::move(context), std::move(filename), max_bytes);
  if (!contents)
    co_return std::unexpected{contents.error()};
  co_return std::string(contents->begin(), contents->end());
}

/** @brief 创建或截断文件后写入全部内容。 */
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

/** @brief 创建或截断文件后写入全部内容。 */
inline task<expected<void>> write(io::io_context context, path filename, io::io_buffer contents) {
  co_return co_await write(std::move(context), std::move(filename), contents.bytes());
}

/** @brief 只在创建请求时读取默认 context；后续操作、路径访问和析构均不依赖执行线程的 TLS。 */
inline auto metadata(path name) {
  return metadata(io::io_context::current(), std::move(name));
}

/** @brief 取得链接自身的元数据，不跟随最终符号链接。 */
inline auto symlink_metadata(path name) {
  return symlink_metadata(io::io_context::current(), std::move(name));
}

/** @brief 设置目标文件的权限。 */
inline auto set_permissions(path name, Permissions value) {
  return set_permissions(io::io_context::current(), std::move(name), value);
}

/** @brief 解析符号链接并返回规范化绝对路径。 */
inline auto canonicalize(path name) {
  return canonicalize(io::io_context::current(), std::move(name));
}

/** @brief 将路径重命名或移动到目标位置。 */
inline auto rename(path from, path to) {
  return rename(io::io_context::current(), std::move(from), std::move(to));
}

/** @brief 删除一个文件或符号链接路径。 */
inline auto remove_file(path name) {
  return remove_file(io::io_context::current(), std::move(name));
}

/** @brief 为已有文件创建硬链接。 */
inline auto hard_link(path from, path to) {
  return hard_link(io::io_context::current(), std::move(from), std::move(to));
}

/** @brief 创建指向指定目标的符号链接。 */
inline auto symlink(path from, path to) {
  return symlink(io::io_context::current(), std::move(from), std::move(to));
}

/** @brief 创建单个目录。 */
inline auto create_dir(path name, Permissions value = Permissions{std::filesystem::perms::all}) {
  return create_dir(io::io_context::current(), std::move(name), value);
}

/** @brief 创建路径中缺少的各级目录。 */
inline auto create_dir_all(path name) {
  return create_dir_all(io::io_context::current(), std::move(name));
}

/** @brief 删除一个空目录。 */
inline auto remove_dir(path name) {
  return remove_dir(io::io_context::current(), std::move(name));
}

/** @brief 递归删除目录树，遵循删除选项及链接边界。 */
inline auto remove_dir_all(path name, recursive_remove_options options = {}) {
  return remove_dir_all(io::io_context::current(), std::move(name), options);
}

/** @brief 复制文件内容并返回复制的字节数。 */
inline auto copy(path from, path to) {
  return copy(io::io_context::current(), std::move(from), std::move(to));
}

/** @brief 将完整文件读入自有缓冲区，并检查字节上限。 */
inline auto read(path name, std::size_t limit = 256 * 1024 * 1024) {
  return read(io::io_context::current(), std::move(name), limit);
}

/** @brief 读取字节并追加到字符串，按已完成的字节进度传播错误。 */
inline auto read_to_string(path name, std::size_t limit = 256 * 1024 * 1024) {
  return read_to_string(io::io_context::current(), std::move(name), limit);
}

/** @brief 创建或截断文件后写入全部内容。 */
inline auto write(path name, std::span<const char> contents) {
  return write(io::io_context::current(), std::move(name), contents);
}

/** @brief 创建或截断文件后写入全部内容。 */
inline auto write(path name, io::io_buffer contents) {
  return write(io::io_context::current(), std::move(name), std::move(contents));
}

/** @brief 创建或截断文件后写入全部内容。 */
inline auto write(path name, std::string_view contents) {
  return write(io::io_context::current(),
               std::move(name),
               std::span<const char>{contents.data(), contents.size()});
}

/**  @brief 可复用的目录创建配置，权限只影响本次新建目录。 */
class DirBuilder {
 public:
  /** @brief 设置目录构建器是否递归创建父目录。 */
  DirBuilder& recursive(bool enabled = true) noexcept {
    recursive_ = enabled;
    return *this;
  }

  /** @brief 设置新目录的权限选项。 */
  DirBuilder& permissions(Permissions value) noexcept {
    permissions_ = value;
    return *this;
  }

  /** @brief 按构建器选项创建目录。 */
  auto create(io::io_context context, path name) const {
    return recursive_ ? create_dir_all(std::move(context), std::move(name), permissions_)
                      : create_dir(std::move(context), std::move(name), permissions_);
  }

  /** @brief 按构建器选项创建目录。 */
  auto create(path name) const { return create(io::io_context::current(), std::move(name)); }

 private:
  bool recursive_{};                                     /**< 是否递归处理目标目录及其子目录。 */
  Permissions permissions_{std::filesystem::perms::all}; /**< 创建文件或目录时使用的权限配置。 */
};
}  // namespace faio::fs
