/**
 * @file faio/detail/fs/platform/linux/directory_cursor.hpp
 * @brief 文件与目录异步接口：原生目录游标。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/fs/provider/file_request.hpp"
#include <array>
#include <cstring>
#include <dirent.h>
#include <sys/syscall.h>

namespace faio::fs::detail {
/** @brief Linux目录fd及有界枚举缓存；OPEN/CLOSE原生，getdents64单独fallback。
 * @details 当前内核没有GETDENTS opcode。fill仅在隔离文件服务调用，take只解析
 *          已有缓存，不发系统调用；其余目录操作不借用该服务。
 */
struct native_directory_cursor {
  struct entry {
    std::string name;  ///< 拷贝后的文件名，不借用下一次fill将覆盖的缓存。
    unsigned char type{};  ///< d_type仅作提示，安全判断使用fd-relative STATX。
  };

  static constexpr std::size_t buffer_size = 4096;  ///< 单次枚举内存/系统调用预算。
  io::io_context context;  ///< 固定目录的原生关闭归属，不读取恢复线程TLS。
  int descriptor{-1};  ///< release以后为-1，只有一个调用者接管关闭责任。
  std::array<char, buffer_size> buffer{};  ///< getdents64输出由游标拥有。
  std::size_t position{},  ///< 当前逻辑游标的字节偏移；更新受所属游标锁保护。
      size{};  ///< 当前有效数据的数量。
  bool eof{};  ///< 零字节结果以后不再重复枚举系统调用。

  native_directory_cursor(io::io_context owner, int fd)
      : context(std::move(owner)), descriptor(fd) {}

  native_directory_cursor(const native_directory_cursor&) = delete;

  /** @brief 异常或取消退出时接管唯一fd，优先原生CLOSE并等待其完成责任排空。 */
  ~native_directory_cursor() {
    /// @brief 正常close已接管时不再次关闭可能被重用的整数fd。
    const int fd = release();
    /// @brief 仅缺少CLOSE能力时使用保留清理服务。
    if (fd >= 0 && !context.domain()->defer_native_close(fd))
      context.defer_cleanup([fd] { ::close(fd); });
  }

  /** @brief 单次接管句柄；后续析构与显式close均观察到-1。 */
  int release() noexcept { return std::exchange(descriptor, -1); }

  /** @brief 缓存耗尽时唯一的目录枚举fallback；一次读至多4KiB。 */
  expected<void> fill() {
    /// @brief 不覆盖尚未消费的目录记录，也不在EOF后重新开始枚举。
    if (position != size || eof)
      return {};
    /// @brief 使用有符号结果，错误不能转换为巨大的缓存长度。
    long count;
    do {
      /// @brief 仅此调用在文件服务线程执行。
      count = ::syscall(SYS_getdents64,
                        descriptor,
                        buffer.data(),
                        buffer.size());
    /// @brief 中断没有交付数据时重试同一目录游标。
    } while (count < 0 && errno == EINTR);
    /// @brief 保留系统错误，不能把失败当EOF。
    if (count < 0)
      return std::unexpected{make_error(errno)};
    /// @brief 新批次从第一条记录开始解析。
    position = 0;
    /// @brief 仅解析内核确实写入的范围。
    size = static_cast<std::size_t>(count);
    /// @brief 零字节才是终点，短批次仍允许继续枚举。
    eof = !size;
    return {};
  }

  /** @brief 只解析已有记录；空结果且!eof表示需要fill，不在worker执行枚举。 */
  expected<std::optional<entry>> take() {
    for (;;) {
      /// @brief 调用方根据eof区分补充缓存和枚举结束。
      if (position == size)
        return std::optional<entry>{};
      /// @brief linux_dirent64 ABI：ino/off各8字节，reclen2字节，type1字节，随后name。
      constexpr std::size_t name_offset = 19;
      /// @brief 先验证固定头及至少一个名称字节。
      if (size - position <= name_offset)
        return std::unexpected{make_error(EIO)};
      /// @brief 后续读取均限制在当前有效批次。
      const char* record = buffer.data() + position;
      std::uint16_t length;
      /// @brief 不依赖C++对象别名或对齐。
      std::memcpy(&length, record + 16,
                  sizeof(length));
      /// @brief 拒绝零长、截断或越过缓存尾部的记录。
      if (length <= name_offset || length > size - position)
        return std::unexpected{make_error(EIO)};
      /// @brief 不把变长ABI映射成不满足对齐的C++结构。
      const char* name = record + name_offset;
      /// @brief 名称终止符必须在记录内部。
      const char* end = static_cast<const char*>(
          std::memchr(name, 0, length - name_offset));
      /// @brief 禁止string构造越界寻找终止符。
      if (!end)
        return std::unexpected{make_error(EIO)};
      /// @brief 下一条仍限于本次getdents64实际写入范围。
      position += length;
      /// @brief 返回拥有型名称，下一批fill不能使结果悬空。
      std::string owned{name, end};
      /// @brief 递归遍历不得经这两项回到本目录或父目录。
      if (owned == "." || owned == "..")
        continue;
      return std::optional<entry>{entry{std::move(owned), static_cast<unsigned char>(record[18])}};
    }
  }
};

/** @brief 原生打开目录；失败或分配异常时仍把已取得fd交回原生关闭管线。 */
inline task<expected<std::shared_ptr<native_directory_cursor>>> open_native_directory(
    io::io_context context, int parent, std::string filename, bool no_follow) {
  io::detail::io_request request;
  /// @brief 直接映射原生OPENAT，不创建文件服务job。
  request.kind = io::detail::operation_kind::open;
  /// @brief 以已打开父目录为锚点，避免拼接绝对路径再解析。
  request.fd = parent;
  /// @brief 稳定请求拥有路径，提交后不借用临时字符串。
  request.path = std::move(filename);
  /// @brief 递归删除时禁止跟随符号链接。
  request.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC
                  | (no_follow ? O_NOFOLLOW : 0);
  /// @brief 等待真实CQE后才接管fd。
  auto opened =
      co_await io::native_file_op(context, std::move(request));
  if (!opened)
    co_return std::unexpected{opened.error()};
  /// @brief 原生OPENAT成功结果是新句柄。
  const int fd = static_cast<int>(*opened);
  try {
    /// @brief 共享游标覆盖异步fill及关闭的生命周期。
    co_return std::make_shared<native_directory_cursor>(
        context, fd);
  } catch (const std::bad_alloc&) {
    /// @brief 分配失败仍必须保存已生成fd的清理责任。
    if (!context.domain()->defer_native_close(fd))
      context.defer_cleanup([fd] { ::close(fd); });
    co_return std::unexpected{make_error(ENOMEM)};
  }
}

/** @brief 不可取消的原生目录CLOSE，接管fd后等待唯一最终CQE。 */
inline auto close_native_directory(const std::shared_ptr<native_directory_cursor>& cursor) {
  io::detail::io_request request;
  /// @brief 直接使用原生关闭awaiter，无转发协程。
  request.kind = io::detail::operation_kind::close;
  /// @brief 析构从此不再关闭同一个fd。
  request.fd = cursor->release();
  /// @brief 不可取消；最终CLOSE CQE才返回。
  return io::native_file_close(cursor->context,
                               std::move(request));
}
}
