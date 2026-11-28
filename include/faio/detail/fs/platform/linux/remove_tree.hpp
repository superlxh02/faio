/**
 * @file faio/detail/fs/platform/linux/remove_tree.hpp
 * @brief 递归删除遍历。
 */
#pragma once
#include "faio/detail/coroutine/context/this_coro.hpp"
#include "faio/detail/fs/core/remove_options.hpp"
#include "faio/detail/fs/platform/linux/directory_cursor.hpp"
#include "faio/detail/fs/platform/linux/native_metadata.hpp"
#include "faio/detail/fs/provider/file_request.hpp"
#include <vector>

namespace faio::fs::detail {
/**
 *  @brief 相对于 fd 的删除请求；使用原生 UNLINKAT 提交，仅在操作码不可用时选择服务。
 * @param context 拥有本次请求和取消、关闭责任的 IO 域。
 * @param parent 解析 filename 的父目录 fd；AT_FDCWD 表示进程工作目录。
 * @param filename 由请求持有的名称，在最终 CQE 到达前不借用枚举缓存。
 * @param flags AT_REMOVEDIR 删除目录；零删除非目录或符号链接本身。
 * @return 成功完成一项删除的直接 awaitable；错误进度保留已发生的副作用。
 */
inline auto unlink_relative(io::io_context context, int parent, std::string filename, int flags) {
  struct unlink_operation {
    int parent;           /**< 保持打开的父目录描述符，删除子项时采用相对路径操作。 */
    std::string filename; /**< 相对于父目录的名称副本，避免借用可变枚举缓冲区。 */
    int flags;            /**< 本次原生操作的标志位，按对应系统调用语义解释。 */

    /** @brief 返回当前文件请求的操作类型。 */
    io::detail::operation_kind kind() const noexcept {
      return io::detail::operation_kind::UNLINKAT;
    }

    /** @brief 将紧凑请求展开为完整请求，固定地址、移交资源租约并初始化低频字段。 */
    io::detail::io_request into_request() && {
      io::detail::io_request request;
      request.kind = kind();
      request.fd = parent;
      request.path = std::move(filename);
      request.flags = flags;
      return request;
    }

    /** @brief 执行当前文件请求的同步回退调用。 */
    expected<std::int64_t> operator()() const noexcept {
      if (::unlinkat(parent, filename.c_str(), flags))
        return std::unexpected{make_error(errno)};
      return 1;
    }
  };

  return void_file_request(std::move(context),
                           unlink_operation{parent, std::move(filename), flags});
}

/**
 *  @brief 通过原生请求按深度优先顺序遍历目录；仅用辅助服务执行没有操作码的 getdents64，以填充缓存。
 * @param context 承担原生请求、回退枚举和停机排空的 IO 上下文。
 * @param root 要删除的根路径；根是符号链接时只删除链接本身。
 * @param options 深度、内存和协作批次的上限，已由公开入口验证。
 * @return 已成功删除的条目数；失败时，Error.progress 保留累计删除数量。
 * @details 父目录 fd 保留到子项删除完成；STATX 不跟随链接，OPENAT 使用 O_NOFOLLOW。
 *          无论正常结束、出错还是取消，目录描述符都通过原生 CLOSE/CQE 流程关闭。
 */
inline task<expected<std::uint64_t>> remove_native_tree(io::io_context context,
                                                        path root,
                                                        recursive_remove_options options) {
  struct frame {
    std::shared_ptr<native_directory_cursor>
        cursor; /**< 当前目录 fd 及有界枚举缓存，由显式栈持有，无需递归协程帧，可统一限制深度和状态内存。 */
    int parent;       /**< 下层帧关闭和删除时，上层帧仍持有这个父目录 fd。 */
    std::string name; /**< 拥有相对名称；缓存下一次填充不会覆盖它。 */
  };

  std::vector<frame> stack;
  std::uint64_t removed = 0;
  std::size_t steps = 0;
  std::size_t bytes = root.native().size();
  auto failure = [&](const error& error) -> expected<std::uint64_t> {
    return std::unexpected{
        error.with_progress(static_cast<std::size_t>(removed) + error.progress())};
  };
  auto stop = co_await this_coro::stop_token();
  auto attributes =
      co_await native_metadata_awaiter{context, AT_FDCWD, root.native(), AT_SYMLINK_NOFOLLOW};
  if (!attributes) {
    if (attributes.error().value() == ENOENT)
      co_return std::uint64_t{0};
    co_return failure(attributes.error());
  }
  if (!attributes->is_dir()) {
    auto result = co_await unlink_relative(context, AT_FDCWD, root.native(), 0);
    if (!result)
      co_return failure(result.error());
    co_return std::uint64_t{1};
  }
  constexpr std::size_t frame_bytes = sizeof(native_directory_cursor) + sizeof(frame);
  if (frame_bytes > options.max_state_bytes - bytes)
    co_return failure(make_error(ENAMETOOLONG));
  auto opened = co_await open_native_directory(context, AT_FDCWD, root.native(), true);
  if (!opened)
    co_return failure(opened.error());
  stack.push_back({std::move(*opened), AT_FDCWD, root.native()});
  bytes += frame_bytes;
  while (!stack.empty()) {
    if (stop.stop_requested() || context.stopped())
      co_return failure(make_error(ECANCELED));
    auto& current = stack.back();
    auto entry = current.cursor->take();
    if (!entry)
      co_return failure(entry.error());
    if (!*entry && !current.cursor->eof) {
      auto cursor = current.cursor;
      auto filled = co_await blocking::call(context, [cursor] { return cursor->fill(); });
      if (!filled)
        co_return failure(filled.error());
      continue;
    }
    if (!*entry) {
      auto completed = std::move(current);
      stack.pop_back();
      bytes -= frame_bytes + (stack.empty() ? 0 : completed.name.size());
      auto closed = co_await close_native_directory(completed.cursor);
      if (!closed)
        co_return failure(closed.error());
      auto result =
          co_await unlink_relative(context, completed.parent, completed.name, AT_REMOVEDIR);
      if (!result)
        co_return failure(result.error());
      ++removed;
      if (++steps >= options.batch_size) {
        steps = 0;
        co_await this_coro::yield();
      } else
        co_await this_coro::yield_if_needed();
      continue;
    }
    const int parent = current.cursor->descriptor;
    auto& name = (*entry)->name;
    auto child = co_await native_metadata_awaiter{context, parent, name, AT_SYMLINK_NOFOLLOW};
    if (!child)
      co_return failure(child.error());
    if (!child->is_dir()) {
      auto result = co_await unlink_relative(context, parent, name, 0);
      if (!result)
        co_return failure(result.error());
      ++removed;
    } else {
      if (stack.size() >= options.max_depth)
        co_return failure(make_error(ELOOP));
      if (frame_bytes > options.max_state_bytes - bytes
          || name.size() > options.max_state_bytes - bytes - frame_bytes)
        co_return failure(make_error(ENAMETOOLONG));
      auto directory = co_await open_native_directory(context, parent, name, true);
      if (!directory)
        co_return failure(directory.error());
      bytes += frame_bytes + name.size();
      stack.push_back({std::move(*directory), parent, std::move(name)});
    }
    if (++steps >= options.batch_size) {
      steps = 0;
      co_await this_coro::yield();
    } else
      co_await this_coro::yield_if_needed();
  }
  co_return removed;
}
}  // namespace faio::fs::detail
