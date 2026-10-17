/**
 * @file faio/detail/fs/platform/unix/operations.hpp
 * @brief 文件与目录异步接口：操作工厂与组合接口。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include "faio/detail/fs/provider/directory_cursor.hpp"
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
/** @brief 路径属性直接提交STATX；缺少原生opcode才使用stat/lstat服务。 */
inline task<expected<Metadata>> metadata_path(io::io_context context, path filename, bool follow) {
#if defined(__linux__)
  if (io::supports_native(context, io::detail::operation_kind::statx))
    co_return co_await native_metadata_awaiter{
        std::move(context), AT_FDCWD, filename.native(), follow ? 0 : AT_SYMLINK_NOFOLLOW};
#endif
  co_return co_await execution::execute(
      std::move(context), [filename = std::move(filename), follow]() -> expected<Metadata> {
        struct stat value{};
        if (follow ? ::stat(filename.c_str(), &value) : ::lstat(filename.c_str(), &value))
          return std::unexpected{make_error(errno)};
        return Metadata{value};
      });
}

/** @brief 创建拥有路径的fd相对请求；第二路径同样拥有内存，不引用临时参数。 */
template <io::detail::operation_kind Kind>
struct path_operation {
  path source;  ///< 文件系统操作的源路径副本。
  path destination;  ///< 目标文件系统路径副本，覆盖异步操作执行期。
  int argument{};  ///< 由具体操作解释的首个原生整数参数。
  int flags{};  ///< 本次原生操作的标志位，按对应系统调用语义解释。

  static constexpr io::detail::operation_kind kind() noexcept { return Kind; }

  /// @brief 把紧凑请求物化为完整稳定请求，移动资源租约并初始化冷字段。
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
}

/** @brief 获取跟随符号链接的属性；uring使用原生STATX。 */
inline auto metadata(io::io_context context, path filename) {
  return detail::metadata_path(std::move(context), std::move(filename), true);
}

/** @brief 获取链接本身属性，保留非 UTF-8 POSIX 路径字节。 */
inline auto symlink_metadata(io::io_context context, path filename) {
  return detail::metadata_path(std::move(context), std::move(filename), false);
}

inline auto set_permissions(io::io_context context, path filename, Permissions permissions) {
  return execution::execute(
      std::move(context), [filename = std::move(filename), permissions]() -> expected<void> {
        if (::chmod(filename.c_str(), static_cast<mode_t>(permissions.bits())))
          return std::unexpected{make_error(errno)};
        return {};
      });
}

inline auto canonicalize(io::io_context context, path filename) {
  return execution::execute(std::move(context),
                            [filename = std::move(filename)]() -> expected<path> {
                              std::error_code error;
                              auto result = std::filesystem::canonical(filename, error);
                              if (error)
                                return std::unexpected{make_error(error.value())};
                              return result;
                            });
}

inline auto rename(io::io_context context, path source, path destination) {
  return detail::void_file_request(std::move(context),
                                   detail::path_operation<io::detail::operation_kind::renameat>{
                                       std::move(source), std::move(destination)});
}

inline auto remove_file(io::io_context context, path filename) {
  return detail::void_file_request(
      std::move(context),
      detail::path_operation<io::detail::operation_kind::unlinkat>{std::move(filename), {}});
}

inline auto hard_link(io::io_context context, path source, path destination) {
  return detail::void_file_request(std::move(context),
                                   detail::path_operation<io::detail::operation_kind::linkat>{
                                       std::move(source), std::move(destination)});
}

inline auto symlink(io::io_context context, path source, path destination) {
  return detail::void_file_request(std::move(context),
                                   detail::path_operation<io::detail::operation_kind::symlinkat>{
                                       std::move(source), std::move(destination)});
}

inline auto create_dir(io::io_context context,
                       path filename,
                       Permissions permissions = Permissions{std::filesystem::perms::all}) {
  return detail::void_file_request(
      std::move(context),
      detail::path_operation<io::detail::operation_kind::mkdirat>{
          std::move(filename), {}, static_cast<int>(permissions.bits())});
}

/** @brief 逐级原生MKDIRAT；遇已有目录用STATX验证，不修改其权限。 */
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

inline auto remove_dir(io::io_context context, path filename) {
  return detail::void_file_request(std::move(context),
                                   detail::path_operation<io::detail::operation_kind::unlinkat>{
                                       std::move(filename), {}, 0, AT_REMOVEDIR});
}

namespace detail {
/** @brief
 * fd相对的深度优先遍历；O_NOFOLLOW和AT_SYMLINK_NOFOLLOW保证不穿越目录链接。 */
struct remove_tree_state {
  struct frame {
    DIR* directory;  ///< 递归遍历时保持打开的原生目录游标。
    int parent;  ///< 父目录保持打开，子目录删除不依赖会变化的完整路径。
    std::string name;  ///< 拥有的目录项名称，避免借用下一批枚举将覆盖的存储。
  };

  io::io_context context;  ///< 持有所属 IO 上下文租约，用于资源归属、服务选择与安全排空。
  path root;  ///< 递归文件操作的根路径副本。
  recursive_remove_options options;  ///< 当前文件操作的选项副本。
  std::vector<frame> stack;  ///< 递归删除的显式遍历栈，避免递归调用栈随目录深度增长。
  std::size_t state_bytes{};  ///< 当前遍历状态占用的字节数，用于执行内存预算。
  bool initialized{},  ///< 遍历或批次状态是否已完成首次初始化。
      done{};  ///< 操作是否已结束，后续推进据此直接返回。

  remove_tree_state(io::io_context owner, path filename, recursive_remove_options limits)
      : context(std::move(owner)),
        root(std::move(filename)),
        options(limits),
        state_bytes(root.native().size()) {}

  ~remove_tree_state() {
    if (stack.empty())
      return;
    /// @brief 错误/取消后的剩余游标在保留清理通道关闭，析构不阻塞运行worker。
    context.defer_cleanup([remaining = std::move(stack)] {
      for (auto it = remaining.rbegin(); it != remaining.rend(); ++it)
        ::closedir(it->directory);
    });
  }

  /** @brief 每步前检查停止；返回本批已删除数，错误进度也是条目数。 */
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
        /// @brief child关闭之后才删除目录；父fd仍由下层frame持有，不会在处理中复用。
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
      /// @brief 在readdir再次复用原生存储前拥有名称。
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

}

/**
 * @brief 有界分批递归删除，不跟随目录符号链接；失败返回累计已删除条目数。
 * @details
 * 删除不是原子事务。取消每步观察，已经开始的短批次排空后才交还请求状态。
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
    auto batch =
        co_await execution::execute(context, [state, stop] { return state->advance(stop); });
    if (!batch)
      co_return std::unexpected{
          Error{batch.error().value(), static_cast<std::size_t>(total) + batch.error().progress()}};
    total += *batch;
    co_await this_coro::yield_if_needed();
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
  /// @brief 先打开而不截断，使用持有fd的inode身份识别同路径、硬链接和符号链接自复制。
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
    co_return std::unexpected{Error{permissions.error().value(), *transferred}};
  auto closed = co_await output->close();
  if (!closed)
    co_return std::unexpected{Error{closed.error().value(), *transferred}};
  auto input_closed = co_await input->close();
  if (!input_closed)
    co_return std::unexpected{Error{input_closed.error().value(), *transferred}};
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
  /// @brief 达到限制时探测一个字节，避免把截断的文件冒充完整结果。
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

/// @brief 读取字节并追加到字符串，按已完成的字节进度传播错误。
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

/// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
inline task<expected<void>> write(io::io_context context, path filename, io::io_buffer contents) {
  co_return co_await write(std::move(context), std::move(filename), contents.bytes());
}

/// @brief 默认 context 只在创建请求时读取；操作、路径与析构不依赖未来线程的 TLS。
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

/// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
inline auto read(path name, std::size_t limit = 256 * 1024 * 1024) {
  return read(io::io_context::current(), std::move(name), limit);
}

/// @brief 读取字节并追加到字符串，按已完成的字节进度传播错误。
inline auto read_to_string(path name, std::size_t limit = 256 * 1024 * 1024) {
  return read_to_string(io::io_context::current(), std::move(name), limit);
}

/// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
inline auto write(path name, std::span<const char> contents) {
  return write(io::io_context::current(), std::move(name), contents);
}

/// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
inline auto write(path name, io::io_buffer contents) {
  return write(io::io_context::current(), std::move(name), std::move(contents));
}

/// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
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
    /// @brief 配置在构造请求时复制；临时DirBuilder的生命周期不必覆盖co_await。
    return recursive_ ? create_dir_all(std::move(context), std::move(name), permissions_)
                      : create_dir(std::move(context), std::move(name), permissions_);
  }

  auto create(path name) const { return create(io::io_context::current(), std::move(name)); }

 private:
  bool recursive_{};  ///< 是否递归处理目标目录及其子目录。
  Permissions permissions_{std::filesystem::perms::all};  ///< 创建文件或目录时使用的权限配置。
};
}
