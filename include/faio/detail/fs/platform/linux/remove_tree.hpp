/**
 * @file faio/detail/fs/platform/linux/remove_tree.hpp
 * @brief 文件与目录异步接口：递归删除遍历。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include "faio/detail/coroutine/this_coro.hpp"
#include "faio/detail/fs/core/remove_options.hpp"
#include "faio/detail/fs/platform/linux/directory_cursor.hpp"
#include "faio/detail/fs/platform/linux/native_metadata.hpp"
#include "faio/detail/fs/provider/file_request.hpp"
#include <vector>

namespace faio::fs::detail {
/** @brief fd 相对的删除请求；UNLINKAT 原生提交，缺 opcode 时才选择服务。
 * @param context 拥有本次请求和取消、关闭责任的 IO 域。
 * @param parent 解析 filename 的父目录 fd；AT_FDCWD 表示进程工作目录。
 * @param filename 由请求拥有的名称，直到最终 CQE 不借用枚举缓存。
 * @param flags AT_REMOVEDIR 删除目录；零删除非目录或符号链接本身。
 * @return 成功完成一项删除的直接 awaitable；错误进度保留已发生的副作用。
 */
inline auto unlink_relative(io::io_context context, int parent, std::string filename, int flags) {
  struct unlink_operation {
    int parent;  ///< 保持打开的父目录描述符，删除子项时采用相对路径操作。
    std::string filename;  ///< 相对于父目录的名称副本，避免借用可变枚举缓冲区。
    int flags;  ///< 本次原生操作的标志位，按对应系统调用语义解释。

    io::detail::operation_kind kind() const noexcept {
      return io::detail::operation_kind::unlinkat;
    }

    /// @brief 把紧凑请求物化为完整稳定请求，移动资源租约并初始化冷字段。
    io::detail::io_request into_request() && {
      io::detail::io_request request;
      request.kind = kind();
      request.fd = parent;
      request.path = std::move(filename);
      request.flags = flags;
      return request;
    }

    expected<std::int64_t> operator()() const noexcept {
      if (::unlinkat(parent, filename.c_str(), flags))
        return std::unexpected{make_error(errno)};
      /// @brief 取消晚于删除时保留真实条目进度。
      return 1;
    }
  };

  return void_file_request(std::move(context),
                           unlink_operation{parent, std::move(filename), flags});
}

/** @brief 原生目录DFS；只有无opcode的getdents64填充缓存使用辅助服务。
 * @param context 承担原生请求、回退枚举和停机排空的 IO 上下文。
 * @param root 要删除的根路径；根是符号链接时只删除链接本身。
 * @param options 深度、内存和协作批次的上限，已由公开入口验证。
 * @return 已成功删除的条目数；失败的 Error.progress 包含累计真实进度。
 * @details 父fd一直保留至子项删除，STATX不跟随链接且OPENAT使用O_NOFOLLOW。
 *          目录描述符的正常、错误和取消关闭都回到原生CLOSE/CQE管线。
 */
inline task<expected<std::uint64_t>> remove_native_tree(io::io_context context,
                                                        path root,
                                                        recursive_remove_options options) {
  struct frame {
    std::shared_ptr<native_directory_cursor> cursor;  ///< 显式栈代替递归协程帧，统一限制深度和状态内存。 当前目录 fd 与有限枚举缓存。
    int parent;  ///< 下层帧关闭和删除时，上层帧仍持有这个父目录 fd。
    std::string name;  ///< 拥有相对名称；缓存下一次填充不会覆盖它。
  };

  /// @brief 栈只存活跃祖先，已删除子树立即释放状态。
  std::vector<frame> stack;
  /// @brief 只在真实成功删除完成后递增。
  std::uint64_t removed = 0;
  /// @brief 每批操作数，用于显式让出协作调度预算。
  std::size_t steps = 0;
  /// @brief 根路径已持有，纳入内存上限。
  std::size_t bytes = root.native().size();
  auto failure = [&](const Error& error) -> expected<std::uint64_t> {
    /// @brief 原请求可能在副作用之后收到取消；该请求进度与此前累计数一起交付。
    return std::unexpected{
        Error{error.value(), static_cast<std::size_t>(removed) + error.progress(), error.domain()}};
  };
  /// @brief 同时观察任务取消和域停机。
  auto stop = co_await this_coro::stop_token();
  /// @brief STATX 不跟随根链接。
  auto attributes = co_await native_metadata_awaiter{
      context, AT_FDCWD, root.native(), AT_SYMLINK_NOFOLLOW};
  if (!attributes) {
    /// @brief 根已不存在符合删除完成语义。
    if (attributes.error().value() == ENOENT)
      co_return std::uint64_t{0};
    co_return failure(attributes.error());
  }
  if (!attributes->is_dir()) {
    /// @brief 非目录包括链接本身，不能进入其目标目录。
    auto result = co_await unlink_relative(context, AT_FDCWD, root.native(), 0);
    if (!result)
      co_return failure(result.error());
    co_return std::uint64_t{1};
  }
  constexpr std::size_t frame_bytes = sizeof(native_directory_cursor) + sizeof(frame);
  /// @brief 入口已验证 bytes 不超过上限。
  if (frame_bytes > options.max_state_bytes - bytes)
    co_return failure(make_error(ENAMETOOLONG));
  auto opened = co_await open_native_directory(context, AT_FDCWD, root.native(), true);
  /// @brief O_DIRECTORY/O_NOFOLLOW 由打开请求设置，抵御 STATX 与 OPENAT
  /// 之间的链接替换。
  if (!opened)
    co_return failure(opened.error());
  /// @brief 接管根目录关闭责任。
  stack.push_back({std::move(*opened), AT_FDCWD, root.native()});
  /// @brief 根名称已计入初始 bytes，不重复计算。
  bytes += frame_bytes;
  while (!stack.empty()) {
    /// @brief 后序遍历：子项全部删除后才删除其目录。
    /// @brief 每步取消；析构仍负责原生 CLOSE。
    if (stop.stop_requested() || context.stopped())
      co_return failure(make_error(ECANCELED));
    /// @brief 后续 push/pop 后不再使用此引用。
    auto& current = stack.back();
    /// @brief 解析缓存不会调用系统枚举或辅助服务。
    auto entry = current.cursor->take();
    if (!entry)
      co_return failure(entry.error());
    if (!*entry && !current.cursor->eof) {
      /// @brief 回退任务拥有租约，不借用可能迁移的栈帧。
      auto cursor = current.cursor;
      auto filled = co_await execution::execute(context, [cursor] { return cursor->fill(); });
      if (!filled)
        co_return failure(filled.error());
      /// @brief 仅getdents64缺opcode，缓存填充完成后继续原生操作。
      continue;
    }
    if (!*entry) {
      /// @brief 先拥有游标，弹栈后仍能等待真实 CLOSE CQE。
      auto completed = std::move(current);
      /// @brief 父帧保留到下面的 fd 相对删除完成。
      stack.pop_back();
      /// @brief 根路径仍存活，不扣其初始计量。
      bytes -= frame_bytes
               + (stack.empty() ? 0 : completed.name.size());
      /// @brief 不通过辅助线程关闭。
      auto closed = co_await close_native_directory(completed.cursor);
      if (!closed)
        co_return failure(closed.error());
      auto result =
          co_await unlink_relative(context, completed.parent, completed.name, AT_REMOVEDIR);
      if (!result)
        co_return failure(result.error());
      /// @brief UNLINKAT 成功 CQE 才记账，错误路径不会重复累计。
      ++removed;
      if (++steps >= options.batch_size) {
        /// @brief 重置显式批次预算。
        steps = 0;
        /// @brief 大子树不会长时间独占当前 worker。
        co_await this_coro::yield();
      } else
        /// @brief 小批次仍遵守任务的协作预算。
        co_await this_coro::yield_if_needed();
      continue;
    }
    /// @brief 活跃父游标保证 fd 未被关闭或复用。
    const int parent = current.cursor->descriptor;
    /// @brief 枚举结果拥有名称，整个 STATX/删除等待期间都有效。
    auto& name = (*entry)->name;
    /// @brief 检查链接本身，禁止越出树边界。
    auto child = co_await native_metadata_awaiter{
        context, parent, name, AT_SYMLINK_NOFOLLOW};
    if (!child)
      co_return failure(child.error());
    if (!child->is_dir()) {
      /// @brief 普通文件和链接直接删除，无须增加 DFS 帧。
      auto result = co_await unlink_relative(context, parent, name, 0);
      if (!result)
        co_return failure(result.error());
      /// @brief 本次请求成功后才累计一项。
      ++removed;
    } else {
      /// @brief 入栈前拒绝深度超限。
      if (stack.size() >= options.max_depth)
        co_return failure(make_error(ELOOP));
      /// @brief 先检查固定开销，避免减法下溢。
      if (frame_bytes > options.max_state_bytes - bytes ||
          name.size() > options.max_state_bytes - bytes - frame_bytes)
        co_return failure(make_error(ENAMETOOLONG));
      auto directory = co_await open_native_directory(context, parent, name, true);
      /// @brief O_NOFOLLOW 把检查后被替换成链接的目录作为错误处理，绝不遍历链接目标。
      if (!directory)
        co_return failure(directory.error());
      /// @brief 只累计当前活跃祖先的状态与名称。
      bytes += frame_bytes + name.size();
      /// @brief 移入 owned 名称。
      stack.push_back({std::move(*directory), parent, std::move(name)});
    }
    if (++steps >= options.batch_size) {
      /// @brief 新进入目录也计入公平性批次，避免只有删除动作才让出。
      steps = 0;
      co_await this_coro::yield();
    } else
      co_await this_coro::yield_if_needed();
  }
  /// @brief 栈排空代表每个目录和文件的最终删除都已完成。
  co_return removed;
}
}
