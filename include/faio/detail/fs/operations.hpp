#pragma once
#include "faio/detail/fs/directory_cursor.hpp"
#include "faio/detail/fs/file.hpp"
#include <dirent.h>
#include <filesystem>
#include <string>
#include <vector>

namespace faio::fs {
namespace detail {
/** @brief 路径属性直接提交STATX；缺少原生opcode才使用stat/lstat服务。 */
inline task<expected<Metadata>> metadata_path(io::io_context context,
                                              path filename, bool follow) {
#if defined(__linux__)
  if (io::supports_native(context, io::detail::operation_kind::statx))
    co_return co_await native_metadata_awaiter{
        std::move(context), AT_FDCWD, filename.native(),
        follow ? 0 : AT_SYMLINK_NOFOLLOW};
#endif
  co_return co_await execution::execute(
      std::move(context),
      [filename = std::move(filename), follow]() -> expected<Metadata> {
        struct stat value{};
        if (follow ? ::stat(filename.c_str(), &value)
                   : ::lstat(filename.c_str(), &value))
          return std::unexpected{make_error(errno)};
        return Metadata{value};
      });
}
/** @brief 将原生零结果转换为void；该协程就是路径API的唯一操作帧。 */
template <class F>
task<expected<void>> void_file_request(io::io_context context,
                                       io::detail::io_request request,
                                       F fallback) {
  auto result = co_await execute_file_request(
      std::move(context), std::move(request), std::move(fallback));
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}
/** @brief 创建拥有路径的fd相对请求；第二路径同样拥有内存，不引用临时参数。 */
inline io::detail::io_request path_request(io::detail::operation_kind kind,
                                           const path &filename) {
  io::detail::io_request request;
  request.kind = kind;
  request.fd = AT_FDCWD;
  request.path = filename.native();
  return request;
}
} // namespace detail
/** @brief 获取跟随符号链接的属性；uring使用原生STATX。 */
inline auto metadata(io::io_context context, path filename) {
  return detail::metadata_path(std::move(context), std::move(filename), true);
}
/** @brief 获取链接本身属性，保留非 UTF-8 POSIX 路径字节。 */
inline auto symlink_metadata(io::io_context context, path filename) {
  return detail::metadata_path(std::move(context), std::move(filename), false);
}
inline auto set_permissions(io::io_context context, path filename,
                            Permissions permissions) {
  return execution::execute(
      std::move(context),
      [filename = std::move(filename), permissions]() -> expected<void> {
        if (::chmod(filename.c_str(), static_cast<mode_t>(permissions.bits())))
          return std::unexpected{make_error(errno)};
        return {};
      });
}
inline auto canonicalize(io::io_context context, path filename) {
  return execution::execute(
      std::move(context), [filename = std::move(filename)]() -> expected<path> {
        std::error_code error;
        auto result = std::filesystem::canonical(filename, error);
        if (error)
          return std::unexpected{make_error(error.value())};
        return result;
      });
}
inline auto rename(io::io_context context, path source, path destination) {
  auto request =
      detail::path_request(io::detail::operation_kind::renameat, source);
  request.argument2 = AT_FDCWD;
  request.path2 = destination.native();
  return detail::void_file_request(
      std::move(context), std::move(request),
      [source = std::move(source),
       destination = std::move(destination)]() -> expected<void> {
        if (::rename(source.c_str(), destination.c_str()))
          return std::unexpected{make_error(errno)};
        return {};
      });
}
inline auto remove_file(io::io_context context, path filename) {
  auto request =
      detail::path_request(io::detail::operation_kind::unlinkat, filename);
  return detail::void_file_request(
      std::move(context), std::move(request),
      [filename = std::move(filename)]() -> expected<void> {
        if (::unlink(filename.c_str()))
          return std::unexpected{make_error(errno)};
        return {};
      });
}
inline auto hard_link(io::io_context context, path source, path destination) {
  auto request =
      detail::path_request(io::detail::operation_kind::linkat, source);
  request.argument2 = AT_FDCWD;
  request.path2 = destination.native();
  return detail::void_file_request(
      std::move(context), std::move(request),
      [source = std::move(source),
       destination = std::move(destination)]() -> expected<void> {
        if (::link(source.c_str(), destination.c_str()))
          return std::unexpected{make_error(errno)};
        return {};
      });
}
inline auto symlink(io::io_context context, path source, path destination) {
  auto request =
      detail::path_request(io::detail::operation_kind::symlinkat, source);
  request.path2 = destination.native();
  return detail::void_file_request(
      std::move(context), std::move(request),
      [source = std::move(source),
       destination = std::move(destination)]() -> expected<void> {
        if (::symlink(source.c_str(), destination.c_str()))
          return std::unexpected{make_error(errno)};
        return {};
      });
}
inline auto create_dir(io::io_context context, path filename,
                       Permissions permissions = Permissions{
                           std::filesystem::perms::all}) {
  auto request =
      detail::path_request(io::detail::operation_kind::mkdirat, filename);
  request.argument = static_cast<int>(permissions.bits());
  return detail::void_file_request(
      std::move(context), std::move(request),
      [filename = std::move(filename), permissions]() -> expected<void> {
        if (::mkdir(filename.c_str(), static_cast<mode_t>(permissions.bits())))
          return std::unexpected{make_error(errno)};
        return {};
      });
}
/** @brief 逐级原生MKDIRAT；遇已有目录用STATX验证，不修改其权限。 */
inline task<expected<void>> create_dir_all(
    io::io_context context, path filename,
    Permissions permissions = Permissions{std::filesystem::perms::all}) {
  if (filename.empty())
    co_return std::unexpected{make_error(ENOENT)};
  path prefix;
  std::size_t depth = 0;
  for (const auto &component : filename) {
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
  auto request =
      detail::path_request(io::detail::operation_kind::unlinkat, filename);
  request.flags = AT_REMOVEDIR;
  return detail::void_file_request(
      std::move(context), std::move(request),
      [filename = std::move(filename)]() -> expected<void> {
        if (::rmdir(filename.c_str()))
          return std::unexpected{make_error(errno)};
        return {};
      });
}
/** @brief
 * 递归删除资源上限；深度包含根目录，batch_size限制一个文件job的枚举步数。 */
struct recursive_remove_options {
  std::size_t max_depth{256}; ///< 同时打开的目录数上限，不递归消耗C++调用栈。
  std::size_t max_state_bytes{1024 * 1024}; ///< 根路径及持有名称的总内存上限。
  std::size_t batch_size{64}; ///< 每个job的最大遍历步数；上限4096。
};
namespace detail {
/** @brief
 * fd相对的深度优先遍历；O_NOFOLLOW和AT_SYMLINK_NOFOLLOW保证不穿越目录链接。 */
struct remove_tree_state {
  struct frame {
    DIR *directory;
    int parent; ///< 父目录保持打开，子目录删除不依赖会变化的完整路径。
    std::string name;
  };
  io::io_context context;
  path root;
  recursive_remove_options options;
  std::vector<frame> stack;
  std::size_t state_bytes{};
  bool initialized{}, done{};
  remove_tree_state(io::io_context owner, path filename,
                    recursive_remove_options limits)
      : context(std::move(owner)), root(std::move(filename)), options(limits),
        state_bytes(root.native().size()) {}
  ~remove_tree_state() {
    if (stack.empty())
      return;
    // 错误/取消后的剩余游标在保留清理通道关闭，析构不阻塞运行worker。
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
        const int fd = ::open(root.c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
          return failure(errno);
        auto *directory = ::fdopendir(fd);
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
      auto &current = stack.back();
      errno = 0;
      auto *entry = ::readdir(current.directory);
      if (!entry) {
        if (errno)
          return failure(errno);
        // child关闭之后才删除目录；父fd仍由下层frame持有，不会在处理中复用。
        const int parent = current.parent;
        auto name = std::move(current.name);
        auto *directory = current.directory;
        stack.pop_back();
        state_bytes -= name.size();
        if (::closedir(directory))
          return failure(errno);
        const int result = parent < 0
                               ? ::rmdir(root.c_str())
                               : ::unlinkat(parent, name.c_str(), AT_REMOVEDIR);
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
      std::string owned_name{name}; // 在readdir再次复用原生存储前拥有名称。
      const int fd = ::openat(parent, owned_name.c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0)
        return failure(errno);
      auto *directory = ::fdopendir(fd);
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
#if defined(__linux__)
/** @brief fd 相对的删除请求；UNLINKAT 原生提交，缺 opcode 时才选择服务。
 * @param context 拥有本次请求和取消、关闭责任的 IO 域。
 * @param parent 解析 filename 的父目录 fd；AT_FDCWD 表示进程工作目录。
 * @param filename 由请求拥有的名称，直到最终 CQE 不借用枚举缓存。
 * @param flags AT_REMOVEDIR 删除目录；零删除非目录或符号链接本身。
 * @return 成功完成一项删除的直接 awaitable；错误进度保留已发生的副作用。
 */
inline auto unlink_relative(io::io_context context, int parent,
                            std::string filename, int flags) {
  io::detail::io_request request; // 稳定请求槽随后拥有所有路径和参数。
  request.kind =
      io::detail::operation_kind::unlinkat; // 映射原生 UNLINKAT SQE。
  request.fd = parent;     // 相对父目录解析，不重新拼接可能遭替换的完整路径。
  request.path = filename; // SQE 提交和最终完成前保持名称存储有效。
  request.flags = flags;   // 原样传递内核要求的文件或目录删除语义。
  return void_file_request(
      std::move(context), std::move(request),
      [parent, filename = std::move(filename),
       flags]() -> expected<std::int64_t> {
        if (::unlinkat(parent, filename.c_str(),
                       flags)) // 仅预先选择的缺 opcode 回退。
          return std::unexpected{make_error(errno)};
        return 1; // 缺opcode的服务路径也保留已删除条目，取消不能丢失真实副作用。
      });
}
/** @brief 原生目录DFS；只有无opcode的getdents64填充缓存使用辅助服务。
 * @param context 承担原生请求、回退枚举和停机排空的 IO 上下文。
 * @param root 要删除的根路径；根是符号链接时只删除链接本身。
 * @param options 深度、内存和协作批次的上限，已由公开入口验证。
 * @return 已成功删除的条目数；失败的 Error.progress 包含累计真实进度。
 * @details 父fd一直保留至子项删除，STATX不跟随链接且OPENAT使用O_NOFOLLOW。
 *          目录描述符的正常、错误和取消关闭都回到原生CLOSE/CQE管线。
 */
inline task<expected<std::uint64_t>>
remove_native_tree(io::io_context context, path root,
                   recursive_remove_options options) {
  struct frame { // 显式栈代替递归协程帧，统一限制深度和状态内存。
    std::shared_ptr<native_directory_cursor>
        cursor;       // 当前目录 fd 与有限枚举缓存。
    int parent;       // 下层帧关闭和删除时，上层帧仍持有这个父目录 fd。
    std::string name; // 拥有相对名称；缓存下一次填充不会覆盖它。
  };
  std::vector<frame> stack;  // 栈只存活跃祖先，已删除子树立即释放状态。
  std::uint64_t removed = 0; // 只在真实成功删除完成后递增。
  std::size_t steps = 0;     // 每批操作数，用于显式让出协作调度预算。
  std::size_t bytes = root.native().size(); // 根路径已持有，纳入内存上限。
  auto failure = [&](const Error &error) -> expected<std::uint64_t> {
    // 原请求可能在副作用之后收到取消；该请求进度与此前累计数一起交付。
    return std::unexpected{Error{
        error.value(), static_cast<std::size_t>(removed) + error.progress(),
        error.domain()}};
  };
  auto stop = co_await this_coro::stop_token(); // 同时观察任务取消和域停机。
  auto attributes = co_await native_metadata_awaiter{
      context, AT_FDCWD, root.native(),
      AT_SYMLINK_NOFOLLOW}; // STATX 不跟随根链接。
  if (!attributes) {
    if (attributes.error().value() == ENOENT) // 根已不存在符合删除完成语义。
      co_return std::uint64_t{0};
    co_return failure(attributes.error());
  }
  if (!attributes->is_dir()) { // 非目录包括链接本身，不能进入其目标目录。
    auto result = co_await unlink_relative(context, AT_FDCWD, root.native(), 0);
    if (!result)
      co_return failure(result.error());
    co_return std::uint64_t{1};
  }
  constexpr std::size_t frame_bytes =
      sizeof(native_directory_cursor) + sizeof(frame);
  if (frame_bytes >
      options.max_state_bytes - bytes) // 入口已验证 bytes 不超过上限。
    co_return failure(make_error(ENAMETOOLONG));
  auto opened =
      co_await open_native_directory(context, AT_FDCWD, root.native(), true);
  // O_DIRECTORY/O_NOFOLLOW 由打开请求设置，抵御 STATX 与 OPENAT
  // 之间的链接替换。
  if (!opened)
    co_return failure(opened.error());
  stack.push_back(
      {std::move(*opened), AT_FDCWD, root.native()}); // 接管根目录关闭责任。
  bytes += frame_bytes;    // 根名称已计入初始 bytes，不重复计算。
  while (!stack.empty()) { // 后序遍历：子项全部删除后才删除其目录。
    if (stop.stop_requested() ||
        context.stopped()) // 每步取消；析构仍负责原生 CLOSE。
      co_return failure(make_error(ECANCELED));
    auto &current = stack.back();        // 后续 push/pop 后不再使用此引用。
    auto entry = current.cursor->take(); // 解析缓存不会调用系统枚举或辅助服务。
    if (!entry)
      co_return failure(entry.error());
    if (!*entry && !current.cursor->eof) {
      auto cursor = current.cursor; // 回退任务拥有租约，不借用可能迁移的栈帧。
      auto filled = co_await execution::execute(
          context, [cursor] { return cursor->fill(); });
      if (!filled)
        co_return failure(filled.error());
      continue; // 仅getdents64缺opcode，缓存填充完成后继续原生操作。
    }
    if (!*entry) {
      auto completed =
          std::move(current); // 先拥有游标，弹栈后仍能等待真实 CLOSE CQE。
      stack.pop_back();       // 父帧保留到下面的 fd 相对删除完成。
      bytes -= frame_bytes +
               (stack.empty()
                    ? 0
                    : completed.name.size()); // 根路径仍存活，不扣其初始计量。
      auto closed = co_await close_native_directory(
          completed.cursor); // 不通过辅助线程关闭。
      if (!closed)
        co_return failure(closed.error());
      auto result = co_await unlink_relative(context, completed.parent,
                                             completed.name, AT_REMOVEDIR);
      if (!result)
        co_return failure(result.error());
      ++removed; // UNLINKAT 成功 CQE 才记账，错误路径不会重复累计。
      if (++steps >= options.batch_size) {
        steps = 0;                   // 重置显式批次预算。
        co_await this_coro::yield(); // 大子树不会长时间独占当前 worker。
      } else
        co_await this_coro::yield_if_needed(); // 小批次仍遵守任务的协作预算。
      continue;
    }
    const int parent =
        current.cursor->descriptor; // 活跃父游标保证 fd 未被关闭或复用。
    auto &name =
        (*entry)->name; // 枚举结果拥有名称，整个 STATX/删除等待期间都有效。
    auto child = co_await native_metadata_awaiter{
        context, parent, name,
        AT_SYMLINK_NOFOLLOW}; // 检查链接本身，禁止越出树边界。
    if (!child)
      co_return failure(child.error());
    if (!child->is_dir()) { // 普通文件和链接直接删除，无须增加 DFS 帧。
      auto result = co_await unlink_relative(context, parent, name, 0);
      if (!result)
        co_return failure(result.error());
      ++removed; // 本次请求成功后才累计一项。
    } else {
      if (stack.size() >= options.max_depth) // 入栈前拒绝深度超限。
        co_return failure(make_error(ELOOP));
      if (frame_bytes > options.max_state_bytes -
                            bytes || // 先检查固定开销，避免减法下溢。
          name.size() > options.max_state_bytes - bytes - frame_bytes)
        co_return failure(make_error(ENAMETOOLONG));
      auto directory =
          co_await open_native_directory(context, parent, name, true);
      // O_NOFOLLOW 把检查后被替换成链接的目录作为错误处理，绝不遍历链接目标。
      if (!directory)
        co_return failure(directory.error());
      bytes += frame_bytes + name.size(); // 只累计当前活跃祖先的状态与名称。
      stack.push_back({std::move(*directory), parent,
                       std::move(name)}); // 移入 owned 名称。
    }
    if (++steps >= options.batch_size) {
      steps = 0; // 新进入目录也计入公平性批次，避免只有删除动作才让出。
      co_await this_coro::yield();
    } else
      co_await this_coro::yield_if_needed();
  }
  co_return removed; // 栈排空代表每个目录和文件的最终删除都已完成。
}
#endif
} // namespace detail
/**
 * @brief 有界分批递归删除，不跟随目录符号链接；失败返回累计已删除条目数。
 * @details
 * 删除不是原子事务。取消每步观察，已经开始的短批次排空后才交还请求状态。
 */
inline task<expected<std::uint64_t>>
remove_dir_all(io::io_context context, path filename,
               recursive_remove_options options = {}) {
  if (!options.max_depth || !options.batch_size || options.batch_size > 4096 ||
      filename.native().size() > options.max_state_bytes)
    co_return std::unexpected{make_error(EINVAL)};
#if defined(__linux__)
  if (io::supports_native(context, io::detail::operation_kind::open) &&
      io::supports_native(context, io::detail::operation_kind::statx))
    co_return co_await detail::remove_native_tree(context, std::move(filename),
                                                  options);
#endif
  auto state = std::make_shared<detail::remove_tree_state>(
      context, std::move(filename), options);
  auto stop = co_await this_coro::stop_token();
  std::uint64_t total = 0;
  while (!state->done) {
    auto batch = co_await execution::execute(
        context, [state, stop] { return state->advance(stop); });
    if (!batch)
      co_return std::unexpected{
          Error{batch.error().value(),
                static_cast<std::size_t>(total) + batch.error().progress()}};
    total += *batch;
    co_await this_coro::yield_if_needed();
  }
  co_return total;
}

/** @brief 有界块复制；每块让出协作预算，并保留失败前写入的字节进度。 */
inline task<expected<std::uint64_t>> copy(io::io_context context, path source,
                                          path destination) {
  auto input = co_await File::open(context, std::move(source));
  if (!input)
    co_return std::unexpected{input.error()};
  auto source_metadata = co_await input->metadata();
  if (!source_metadata)
    co_return std::unexpected{source_metadata.error()};
  if (!source_metadata->is_file())
    co_return std::unexpected{make_error(EINVAL)};
  // 先打开而不截断，使用持有fd的inode身份识别同路径、硬链接和符号链接自复制。
  auto output = co_await File::open(context, std::move(destination),
                                    OpenOptions{}.write().create());
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
  auto permissions =
      co_await output->set_permissions(source_metadata->permissions());
  if (!permissions)
    co_return std::unexpected{Error{permissions.error().value(), *transferred}};
  auto closed = co_await output->close();
  if (!closed)
    co_return std::unexpected{Error{closed.error().value(), *transferred}};
  auto input_closed = co_await input->close();
  if (!input_closed)
    co_return std::unexpected{
        Error{input_closed.error().value(), *transferred}};
  co_return transferred;
}
/** @brief 读完整文件；max_bytes 限制应用内存，不在一次 job 内分配任意大小。 */
inline task<expected<std::vector<char>>>
read(io::io_context context, path filename,
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
inline task<expected<std::string>>
read_to_string(io::io_context context, path filename,
               std::size_t max_bytes = 256 * 1024 * 1024) {
  auto contents =
      co_await read(std::move(context), std::move(filename), max_bytes);
  if (!contents)
    co_return std::unexpected{contents.error()};
  co_return std::string(contents->begin(), contents->end());
}
/** @brief 借用写入，等待关闭后才返回；输入必须覆盖整个等待期间。 */
inline task<expected<void>> write(io::io_context context, path filename,
                                  std::span<const char> contents) {
  auto output = co_await File::create(std::move(context), std::move(filename));
  if (!output)
    co_return std::unexpected{output.error()};
  auto written = co_await output->write_all(contents);
  if (!written)
    co_return std::unexpected{written.error()};
  co_return co_await output->close();
}
inline task<expected<void>> write(io::io_context context, path filename,
                                  io::io_buffer contents) {
  co_return co_await write(std::move(context), std::move(filename),
                           contents.bytes());
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
inline auto create_dir(path name, Permissions value = Permissions{
                                      std::filesystem::perms::all}) {
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
  return write(io::io_context::current(), std::move(name),
               std::span<const char>{contents.data(), contents.size()});
}

/** @brief 可复用的目录创建配置，权限只影响本次新建目录。 */
class DirBuilder {
public:
  DirBuilder &recursive(bool enabled = true) noexcept {
    recursive_ = enabled;
    return *this;
  }
  DirBuilder &permissions(Permissions value) noexcept {
    permissions_ = value;
    return *this;
  }
  auto create(io::io_context context, path name) const {
    // 配置在构造请求时复制；临时DirBuilder的生命周期不必覆盖co_await。
    return recursive_
               ? create_dir_all(std::move(context), std::move(name),
                                permissions_)
               : create_dir(std::move(context), std::move(name), permissions_);
  }
  auto create(path name) const {
    return create(io::io_context::current(), std::move(name));
  }

private:
  bool recursive_{};
  Permissions permissions_{std::filesystem::perms::all};
};
} // namespace faio::fs
