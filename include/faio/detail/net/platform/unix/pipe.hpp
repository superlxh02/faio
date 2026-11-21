/**
 * @file faio/detail/net/platform/unix/pipe.hpp
 * @brief 管道与命名管道。
 */
#ifndef FAIO_DETAIL_NET_PLATFORM_UNIX_PIPE_HPP
#define FAIO_DETAIL_NET_PLATFORM_UNIX_PIPE_HPP
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/awaiter/open.hpp"
#include "faio/detail/io/awaiter/read.hpp"
#include "faio/detail/io/awaiter/write.hpp"
#include "faio/detail/io/awaiter/ready.hpp"
#include "faio/detail/io/base/buffer.hpp"
#include "faio/detail/net/base/socket.hpp"
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <sys/stat.h>

namespace faio::net::unix::pipe {
using owned_native_fd = ::faio::net::detail::owned_native_socket;

namespace detail {
using Socket = ::faio::net::detail::Socket;

/**
 *  @brief 导入只接受正确访问方向的 FIFO/匿名管道，拒绝可能阻塞 reactor
 * 的普通文件。
 */
inline auto validate(int fd, bool write_side) -> expected<void> {
  struct stat information{};
  if (::fstat(fd, &information) < 0)
    return std::unexpected{make_error(errno)};
  if (!S_ISFIFO(information.st_mode))
    return std::unexpected{make_error(EINVAL)};
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0)
    return std::unexpected{make_error(errno)};
  if ((write_side && (flags & O_ACCMODE) == O_RDONLY)
      || (!write_side && (flags & O_ACCMODE) == O_WRONLY))
    return std::unexpected{make_error(EBADF)};
  return {};
}

/** @brief 设置管道所需的非阻塞及禁止继承标志。 */
inline auto prepare(int fd) -> expected<void> {
  return ::faio::net::detail::prepare_native_pipe(fd);
}
}  // namespace detail

/**  @brief 非阻塞 Unix pipe/FIFO 发送端。写端关闭以后对端在排空数据后读到 EOF。 */
class Sender {
 public:
  /** @brief 接管管道写端及所属 IO 上下文。 */
  explicit Sender(detail::Socket socket) : socket_{std::move(socket)} {}

  /** @brief 接管源对象的状态和资源租约。 */
  Sender(Sender&&) noexcept = default;

  /** @brief 移动赋值资源及状态。 */
  Sender& operator=(Sender&&) noexcept = default;

  /** @brief 禁止复制此对象。 */
  Sender(const Sender&) = delete;

  /** @brief 禁止复制赋值。 */
  Sender& operator=(const Sender&) = delete;

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto fd() const noexcept -> int { return socket_.fd(); }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto as_native_handle() const noexcept -> int { return fd(); }

  /** @brief 取得底层共享资源控制块，保持句柄身份及所属域有效。 */
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }

  /** @brief 取得该对象关联的 I/O 上下文租约。 */
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }

  /** @brief 等待 socket 或管道可写，不执行实际写入。 */
  auto writable() const noexcept { return io::ready(resource(), io::Interest::writable); }

  /** @brief 写入一段字节并返回本次实际写入数量。 */
  auto write(std::span<const char> buffer) const noexcept {
    return io::write(
               resource(), buffer.data(), buffer.size(), std::numeric_limits<std::uint64_t>::max())
        .empty_success();
  }

  /** @brief 写入一段字节并返回本次实际写入数量。 */
  auto write(io::borrowed_const_buffer buffer) const noexcept { return write(buffer.bytes); }

  /** @brief 写入一段字节并返回本次实际写入数量。 */
  auto write(io::io_buffer buffer) const -> task<expected<io::io_transfer>> {
    return write_owned(resource(), std::move(buffer));
  }

  /**  @brief 完整写入；大于 PIPE_BUF 的消息不承诺多写者之间的原子性。 */
  auto write_all(std::span<const char> buffer) const -> task<expected<void>> {
    return write_all_impl(resource(), buffer);
  }

  /** @brief 完成刷新；管道发送端没有用户态写缓存。 */
  auto flush() const -> task<expected<void>> { co_return expected<void>{}; }

  /** @brief 异步关闭共享管道端点，等待在途操作安全结束。 */
  auto close() noexcept { return socket_.close(); }

  /** @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。 */
  [[nodiscard]] auto into_native() -> expected<owned_native_fd> { return socket_.into_native(); }

  /** @brief 接管显式传入的原生句柄，按平台规则建立资源包装。 */
  [[nodiscard]] static auto from_native(io::io_context context, owned_native_fd native)
      -> expected<Sender> {
    if (auto result = detail::validate(native.get(), true); !result)
      return std::unexpected{result.error()};
    if (auto result = detail::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = detail::Socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return Sender{std::move(*socket)};
  }

  /**
   *  @brief O_NONBLOCK 打开 FIFO；没有接收端时保留内核 ENXIO。
   * @details io_uring 使用原生 OPENAT SQE/CQE；epoll/kqueue 的文件打开由
   *          通用 IO 引擎交给文件服务，网络层不额外创建辅助线程任务。
   */
  static auto open(io::io_context context, std::string path) -> task<expected<Sender>> {
    if (path.find('\0') != std::string::npos)
      co_return std::unexpected{make_error(EINVAL)};
    auto descriptor =
        co_await io::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC).with_context(context);
    if (!descriptor)
      co_return std::unexpected{descriptor.error()};
    owned_native_fd native{*descriptor};
    co_return from_native(std::move(context), std::move(native));
  }

 private:
  /**  @brief 创建 task 时按值保存资源，延迟启动不再依赖 Sender 对象地址。 */
  static auto write_owned(std::shared_ptr<io::detail::resource_state> lease, io::io_buffer buffer)
      -> task<expected<io::io_transfer>> {
    auto result = co_await io::write(std::move(lease),
                                     buffer.data(),
                                     buffer.size(),
                                     std::numeric_limits<std::uint64_t>::max())
                      .empty_success();
    if (!result)
      co_return std::unexpected{result.error()};
    co_return io::io_transfer{std::move(buffer), *result};
  }

  /** @brief 在持有写方向身份期间循环完成全部字节写入。 */
  static auto write_all_impl(std::shared_ptr<io::detail::resource_state> lease,
                             std::span<const char> buffer) -> task<expected<void>> {
    if (buffer.empty())
      co_return expected<void>{};
    char owner_identity{};
    auto reservation =
        io::detail::reserve_direction(lease, io::Interest::writable, &owner_identity);
    if (!reservation)
      co_return std::unexpected{reservation.error()};
    std::size_t completed{};
    while (!buffer.empty()) {
      auto result =
          co_await io::write(
              lease, buffer.data(), buffer.size(), std::numeric_limits<std::uint64_t>::max())
              .reservation(&owner_identity)
              .empty_success();
      if (!result)
        co_return std::unexpected{
            result.error().with_progress(completed + result.error().progress())};
      if (!*result)
        co_return std::unexpected{Error{Error::WriteZero, completed, error_domain::faio}};
      completed += *result;
      buffer = buffer.subspan(*result);
      co_await this_coro::yield_if_needed();
    }
    co_return expected<void>{};
  }

  detail::Socket socket_; /**< 持有底层管道包装，在挂起期间保持原生资源身份有效。 */
};

/**  @brief 非阻塞 Unix pipe/FIFO 接收端；使用 read 而非 socket recv。 */
class Receiver {
 public:
  /** @brief 接管管道读端及所属 IO 上下文。 */
  explicit Receiver(detail::Socket socket) : socket_{std::move(socket)} {}

  /** @brief 接管源对象的状态和资源租约。 */
  Receiver(Receiver&&) noexcept = default;

  /** @brief 移动赋值资源及状态。 */
  Receiver& operator=(Receiver&&) noexcept = default;

  /** @brief 禁止复制此对象。 */
  Receiver(const Receiver&) = delete;

  /** @brief 禁止复制赋值。 */
  Receiver& operator=(const Receiver&) = delete;

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto fd() const noexcept -> int { return socket_.fd(); }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto as_native_handle() const noexcept -> int { return fd(); }

  /** @brief 取得底层共享资源控制块，保持句柄身份及所属域有效。 */
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }

  /** @brief 取得该对象关联的 I/O 上下文租约。 */
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }

  /** @brief 等待 socket 或管道可读，不消费数据。 */
  auto readable() const noexcept { return io::ready(resource(), io::Interest::readable); }

  /** @brief 读取一段字节，非空缓冲区返回零表示 EOF。 */
  auto read(std::span<char> buffer) const noexcept {
    return io::read(
               resource(), buffer.data(), buffer.size(), std::numeric_limits<std::uint64_t>::max())
        .empty_success();
  }

  /** @brief 读取一段字节，非空缓冲区返回零表示 EOF。 */
  auto read(io::borrowed_buffer buffer) const noexcept { return read(buffer.bytes); }

  /** @brief 读取一段字节，非空缓冲区返回零表示 EOF。 */
  auto read(io::io_buffer buffer) const -> task<expected<io::io_transfer>> {
    return read_owned(resource(), std::move(buffer));
  }

  /** @brief 循环读取，直到缓冲区填满、到达 EOF 或失败；失败时保留已完成的进度。 */
  auto read_exact(std::span<char> buffer) const -> task<expected<void>> {
    return read_exact_impl(resource(), buffer);
  }

  /** @brief 异步关闭共享管道端点，等待在途操作安全结束。 */
  auto close() noexcept { return socket_.close(); }

  /** @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。 */
  [[nodiscard]] auto into_native() -> expected<owned_native_fd> { return socket_.into_native(); }

  /** @brief 接管显式传入的原生句柄，按平台规则建立资源包装。 */
  [[nodiscard]] static auto from_native(io::io_context context, owned_native_fd native)
      -> expected<Receiver> {
    if (auto result = detail::validate(native.get(), false); !result)
      return std::unexpected{result.error()};
    if (auto result = detail::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = detail::Socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return Receiver{std::move(*socket)};
  }

  /**
   *  @brief 非阻塞打开 FIFO 接收端，原生 OPENAT 完成后导入所属 IO domain。
   * @details 取消与成功 CQE 竞争时，通用引擎先回收成功创建的 fd 再发布取消；
   *          接收端仅在成功结果返回后建立唯一拥有者。
   */
  static auto open(io::io_context context, std::string path) -> task<expected<Receiver>> {
    if (path.find('\0') != std::string::npos)
      co_return std::unexpected{make_error(EINVAL)};
    auto descriptor =
        co_await io::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC).with_context(context);
    if (!descriptor)
      co_return std::unexpected{descriptor.error()};
    owned_native_fd native{*descriptor};
    co_return from_native(std::move(context), std::move(native));
  }

 private:
  /**  @brief 创建 task 时按值保存资源，拥有 buffer 的初始化范围只在完成后发布。 */
  static auto read_owned(std::shared_ptr<io::detail::resource_state> lease, io::io_buffer buffer)
      -> task<expected<io::io_transfer>> {
    auto result = co_await io::read(std::move(lease),
                                    buffer.data(),
                                    buffer.capacity(),
                                    std::numeric_limits<std::uint64_t>::max())
                      .empty_success();
    if (!result)
      co_return std::unexpected{result.error()};
    buffer.set_size(*result);
    co_return io::io_transfer{std::move(buffer), *result};
  }

  /** @brief 循环读取直至填满缓冲区，失败时保存已完成字节数。 */
  static auto read_exact_impl(std::shared_ptr<io::detail::resource_state> lease,
                              std::span<char> buffer) -> task<expected<void>> {
    if (buffer.empty())
      co_return expected<void>{};
    char owner_identity{};
    auto reservation =
        io::detail::reserve_direction(lease, io::Interest::readable, &owner_identity);
    if (!reservation)
      co_return std::unexpected{reservation.error()};
    std::size_t completed{};
    while (!buffer.empty()) {
      auto result =
          co_await io::read(
              lease, buffer.data(), buffer.size(), std::numeric_limits<std::uint64_t>::max())
              .reservation(&owner_identity)
              .empty_success();
      if (!result)
        co_return std::unexpected{
            result.error().with_progress(completed + result.error().progress())};
      if (!*result)
        co_return std::unexpected{Error{Error::UnexpectedEOF, completed, error_domain::faio}};
      completed += *result;
      buffer = buffer.subspan(*result);
      co_await this_coro::yield_if_needed();
    }
    co_return expected<void>{};
  }

  detail::Socket socket_; /**< 持有底层管道包装，在挂起期间保持原生资源身份有效。 */
};

/**  @brief 创建内核匿名管道，返回发送端与接收端，不创建额外线程。 */
[[nodiscard]] inline auto pair(io::io_context context = io::io_context::current())
    -> expected<std::pair<Sender, Receiver>> {
  int descriptors[2];
  if (::faio::net::detail::create_native_pipe(descriptors) < 0)
    return std::unexpected{make_error(errno)};
  owned_native_fd read_end{descriptors[0]}, write_end{descriptors[1]};
  auto reader = Receiver::from_native(context, std::move(read_end));
  if (!reader)
    return std::unexpected{reader.error()};
  auto writer = Sender::from_native(std::move(context), std::move(write_end));
  if (!writer)
    return std::unexpected{writer.error()};
  return std::pair{std::move(*writer), std::move(*reader)};
}
}  // namespace faio::net::unix::pipe
#endif
