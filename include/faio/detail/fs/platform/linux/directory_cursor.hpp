/**
 * @file faio/detail/fs/platform/linux/directory_cursor.hpp
 * @brief 原生目录游标。
 */
#pragma once
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/fs/provider/file_request.hpp"
#include <array>
#include <cstring>
#include <dirent.h>
#include <sys/syscall.h>

namespace faio::fs::detail {
/**
 *  @brief Linux 目录 fd 及有界枚举缓存；OPEN/CLOSE 使用原生请求，getdents64 单独走回退路径。
 * @details 当前内核没有 GETDENTS 操作码。fill 仅在独立的文件服务中调用；take 只解析
 *          已有缓存，不发起系统调用。其余目录操作不借用该服务。
 */
struct native_directory_cursor {
  /** @brief 原生目录枚举返回的名称和文件类型记录。 */
  struct entry {
    std::string name;     /**< 文件名副本，不借用下一次 fill 将覆盖的缓存。 */
    unsigned char type{}; /**< d_type 仅作为提示，安全检查使用相对于 fd 的 STATX。 */
  };

  static constexpr std::size_t buffer_size = 4096; /**< 单次枚举内存/系统调用预算。 */
  io::io_context context; /**< 保存目录原生关闭操作的归属，不读取恢复线程的 TLS。 */
  int descriptor{-1};     /**< release 后设为 -1，确保只有一个调用方接管关闭责任。 */
  std::array<char, buffer_size> buffer{}; /**< 游标持有 getdents64 的输出。 */
  std::size_t position{}, size{};         /**< 当前有效数据的数量。 */
  bool eof{};                             /**< 枚举返回零字节后不再重复调用系统接口。 */

  /** @brief 初始化原生目录枚举游标。 */
  native_directory_cursor(io::io_context owner, int fd)
      : context(std::move(owner)), descriptor(fd) {}

  /** @brief 禁止复制此对象。 */
  native_directory_cursor(const native_directory_cursor&) = delete;

  /**  @brief 因异常或取消退出时，接管唯一的 fd，优先提交原生 CLOSE 并等待关闭操作排空。 */
  ~native_directory_cursor() {
    const int fd = release();
    if (fd >= 0 && !context.domain()->defer_native_close(fd))
      context.defer_cleanup([fd] { ::close(fd); });
  }

  /**  @brief 只移交一次句柄，后续析构和显式 close 均读取到 -1。 */
  int release() noexcept { return std::exchange(descriptor, -1); }

  /**  @brief 缓存耗尽时使用的唯一目录枚举回退调用，每次最多读取 4 KiB。 */
  expected<void> fill() {
    if (position != size || eof)
      return {};
    long count;
    do {
      count = ::syscall(SYS_getdents64, descriptor, buffer.data(), buffer.size());
    } while (count < 0 && errno == EINTR);
    if (count < 0)
      return std::unexpected{make_error(errno)};
    position = 0;
    size = static_cast<std::size_t>(count);
    eof = !size;
    return {};
  }

  /**  @brief 只解析已有记录，不在工作线程上执行枚举；结果为空且 !eof 时需要调用 fill。 */
  expected<std::optional<entry>> take() {
    for (;;) {
      if (position == size)
        return std::optional<entry>{};
      constexpr std::size_t name_offset = 19;
      if (size - position <= name_offset)
        return std::unexpected{make_error(EIO)};
      const char* record = buffer.data() + position;
      std::uint16_t length;
      std::memcpy(&length, record + 16, sizeof(length));
      if (length <= name_offset || length > size - position)
        return std::unexpected{make_error(EIO)};
      const char* name = record + name_offset;
      const char* end = static_cast<const char*>(std::memchr(name, 0, length - name_offset));
      if (!end)
        return std::unexpected{make_error(EIO)};
      position += length;
      std::string owned{name, end};
      if (owned == "." || owned == "..")
        continue;
      return std::optional<entry>{entry{std::move(owned), static_cast<unsigned char>(record[18])}};
    }
  }
};

/**  @brief 通过原生请求打开目录；操作失败或分配抛出异常时，仍将已取得的 fd 交回原生关闭流程。 */
inline task<expected<std::shared_ptr<native_directory_cursor>>> open_native_directory(
    io::io_context context, int parent, std::string filename, bool no_follow) {
  io::detail::io_request request;
  request.kind = io::detail::operation_kind::open;
  request.fd = parent;
  request.path = std::move(filename);
  request.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (no_follow ? O_NOFOLLOW : 0);
  auto opened = co_await io::native_file_op(context, std::move(request));
  if (!opened)
    co_return std::unexpected{opened.error()};
  const int fd = static_cast<int>(*opened);
  try {
    co_return std::make_shared<native_directory_cursor>(context, fd);
  } catch (const std::bad_alloc&) {
    if (!context.domain()->defer_native_close(fd))
      context.defer_cleanup([fd] { ::close(fd); });
    co_return std::unexpected{make_error(ENOMEM)};
  }
}

/**  @brief 不可取消的原生目录 CLOSE，接管 fd 后等待唯一的最终 CQE。 */
inline auto close_native_directory(const std::shared_ptr<native_directory_cursor>& cursor) {
  io::detail::io_request request;
  request.kind = io::detail::operation_kind::close;
  request.fd = cursor->release();
  return io::native_file_close(cursor->context, std::move(request));
}
}  // namespace faio::fs::detail
