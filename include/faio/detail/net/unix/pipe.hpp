#ifndef FAIO_DETAIL_NET_UNIX_PIPE_HPP
#define FAIO_DETAIL_NET_UNIX_PIPE_HPP
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/buffer.hpp"
#include "faio/detail/net/common/socket.hpp"
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <sys/stat.h>
// GCC 的 GNU 模式预定义 unix=1，与标准命名空间名冲突，公共头消除此历史宏。
#ifdef unix
#undef unix
#endif

namespace faio::net::unix::pipe {
using owned_native_fd = ::faio::net::detail::owned_native_socket;
namespace detail {
using Socket = ::faio::net::detail::Socket;
/** @brief 导入只接受正确访问方向的 FIFO/匿名管道，拒绝可能阻塞 reactor
 * 的普通文件。 */
inline auto validate(int fd, bool write_side) -> expected<void> {
  struct stat information{};
  if (::fstat(fd, &information) < 0)
    return std::unexpected{make_error(errno)};
  if (!S_ISFIFO(information.st_mode))
    return std::unexpected{make_error(EINVAL)};
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0)
    return std::unexpected{make_error(errno)};
  if ((write_side && (flags & O_ACCMODE) == O_RDONLY) ||
      (!write_side && (flags & O_ACCMODE) == O_WRONLY))
    return std::unexpected{make_error(EBADF)};
  return {};
}
inline auto prepare(int fd) -> expected<void> {
  const int status = ::fcntl(fd, F_GETFL);
  if (status < 0 || ::fcntl(fd, F_SETFL, status | O_NONBLOCK) < 0)
    return std::unexpected{make_error(errno)};
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
    return std::unexpected{make_error(errno)};
#ifdef F_SETNOSIGPIPE
  // Darwin 的 pipe 信号可能发给进程；使用 fd
  // 原生选项抑制，而非修改全局信号处理器。
  if (::fcntl(fd, F_SETNOSIGPIPE, 1) < 0)
    return std::unexpected{make_error(errno)};
#endif
  return {};
}
} // namespace detail
/** @brief 非阻塞 Unix pipe/FIFO 发送端。写端关闭以后对端在排空数据后读到 EOF。
 */
class Sender {
public:
  explicit Sender(detail::Socket socket) : socket_{std::move(socket)} {}
  Sender(Sender &&) noexcept = default;
  Sender &operator=(Sender &&) noexcept = default;
  Sender(const Sender &) = delete;
  Sender &operator=(const Sender &) = delete;
  [[nodiscard]] auto fd() const noexcept -> int { return socket_.fd(); }
  [[nodiscard]] auto as_native_handle() const noexcept -> int { return fd(); }
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }
  auto writable() const noexcept {
    return io::ready(resource(), io::Interest::writable);
  }
  auto write(std::span<const char> buffer) const noexcept {
    return io::write(resource(), buffer.data(), buffer.size(),
                     std::numeric_limits<std::uint64_t>::max())
        .empty_success();
  }
  auto write(io::borrowed_const_buffer buffer) const noexcept {
    return write(buffer.bytes);
  }
  auto write(io::io_buffer buffer) const -> task<expected<io::io_transfer>> {
    return write_owned(resource(), std::move(buffer));
  }
  /** @brief 完整写入；大于 PIPE_BUF 的消息不承诺多写者之间的原子性。 */
  auto write_all(std::span<const char> buffer) const -> task<expected<void>> {
    return write_all_impl(resource(), buffer);
  }
  auto flush() const -> task<expected<void>> { co_return expected<void>{}; }
  auto close() noexcept { return socket_.close(); }
  [[nodiscard]] auto into_native() -> expected<owned_native_fd> {
    return socket_.into_native();
  }
  [[nodiscard]] static auto from_native(io::io_context context,
                                        owned_native_fd native)
      -> expected<Sender> {
    if (auto result = detail::validate(native.get(), true); !result)
      return std::unexpected{result.error()};
    if (auto result = detail::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket =
        detail::Socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return Sender{std::move(*socket)};
  }
  /** @brief O_NONBLOCK 打开 FIFO；没有接收端时保留内核 ENXIO。
   * @details io_uring 使用原生 OPENAT SQE/CQE；epoll/kqueue 的文件打开由
   *          通用 IO 引擎交给文件服务，网络层不额外创建辅助线程任务。
   */
  static auto open(io::io_context context, std::string path)
      -> task<expected<Sender>> {
    if (path.find('\0') != std::string::npos)
      co_return std::unexpected{make_error(EINVAL)};
    // raw awaiter 拥有路径副本，直接提交统一请求；已有 native opcode
    // 不经过线程池。
    auto descriptor =
        co_await io::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC)
            .with_context(context);
    if (!descriptor)
      co_return std::unexpected{descriptor.error()};
    // CQE 成功后立即接管 fd；导入校验或注册失败时只关闭一次。
    owned_native_fd native{*descriptor};
    co_return from_native(std::move(context), std::move(native));
  }

private:
  /** @brief 创建 task 时按值抓取资源，延迟启动不再依赖 Sender 对象地址。 */
  static auto write_owned(std::shared_ptr<io::detail::resource_state> lease,
                          io::io_buffer buffer)
      -> task<expected<io::io_transfer>> {
    auto result =
        co_await io::write(std::move(lease), buffer.data(), buffer.size(),
                           std::numeric_limits<std::uint64_t>::max())
            .empty_success();
    if (!result)
      co_return std::unexpected{result.error()};
    co_return io::io_transfer{std::move(buffer), *result};
  }
  static auto write_all_impl(std::shared_ptr<io::detail::resource_state> lease,
                             std::span<const char> buffer)
      -> task<expected<void>> {
    if (buffer.empty())
      co_return expected<void>{};
    char owner_identity{};
    auto reservation = io::detail::reserve_direction(
        lease, io::Interest::writable, &owner_identity);
    if (!reservation)
      co_return std::unexpected{reservation.error()};
    std::size_t completed{};
    while (!buffer.empty()) {
      auto result =
          co_await io::write(lease, buffer.data(), buffer.size(),
                             std::numeric_limits<std::uint64_t>::max())
              .reservation(&owner_identity)
              .empty_success();
      if (!result)
        co_return std::unexpected{Error{result.error().value(),
                                        completed + result.error().progress(),
                                        result.error().domain()}};
      if (!*result)
        co_return std::unexpected{
            Error{Error::WriteZero, completed, error_domain::faio}};
      completed += *result;
      buffer = buffer.subspan(*result);
      co_await this_coro::yield_if_needed();
    }
    co_return expected<void>{};
  }
  detail::Socket socket_;
};
/** @brief 非阻塞 Unix pipe/FIFO 接收端；使用 read 而非 socket recv。 */
class Receiver {
public:
  explicit Receiver(detail::Socket socket) : socket_{std::move(socket)} {}
  Receiver(Receiver &&) noexcept = default;
  Receiver &operator=(Receiver &&) noexcept = default;
  Receiver(const Receiver &) = delete;
  Receiver &operator=(const Receiver &) = delete;
  [[nodiscard]] auto fd() const noexcept -> int { return socket_.fd(); }
  [[nodiscard]] auto as_native_handle() const noexcept -> int { return fd(); }
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }
  auto readable() const noexcept {
    return io::ready(resource(), io::Interest::readable);
  }
  auto read(std::span<char> buffer) const noexcept {
    return io::read(resource(), buffer.data(), buffer.size(),
                    std::numeric_limits<std::uint64_t>::max())
        .empty_success();
  }
  auto read(io::borrowed_buffer buffer) const noexcept {
    return read(buffer.bytes);
  }
  auto read(io::io_buffer buffer) const -> task<expected<io::io_transfer>> {
    return read_owned(resource(), std::move(buffer));
  }
  auto read_exact(std::span<char> buffer) const -> task<expected<void>> {
    return read_exact_impl(resource(), buffer);
  }
  auto close() noexcept { return socket_.close(); }
  [[nodiscard]] auto into_native() -> expected<owned_native_fd> {
    return socket_.into_native();
  }
  [[nodiscard]] static auto from_native(io::io_context context,
                                        owned_native_fd native)
      -> expected<Receiver> {
    if (auto result = detail::validate(native.get(), false); !result)
      return std::unexpected{result.error()};
    if (auto result = detail::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket =
        detail::Socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return Receiver{std::move(*socket)};
  }
  /** @brief 非阻塞打开 FIFO 接收端，原生 OPENAT 完成后导入所属 IO domain。
   * @details 取消与成功 CQE 竞争时，通用引擎先回收成功创建的 fd 再发布取消；
   *          接收端仅在成功结果返回后建立唯一拥有者。
   */
  static auto open(io::io_context context, std::string path)
      -> task<expected<Receiver>> {
    if (path.find('\0') != std::string::npos)
      co_return std::unexpected{make_error(EINVAL)};
    // OPENAT 的路径存储位于稳定请求槽，挂起期间不借用函数参数的字符串地址。
    auto descriptor =
        co_await io::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)
            .with_context(context);
    if (!descriptor)
      co_return std::unexpected{descriptor.error()};
    // descriptor 已离开内核完成管线，导入失败由 RAII guard 安全关闭。
    owned_native_fd native{*descriptor};
    co_return from_native(std::move(context), std::move(native));
  }

private:
  /** @brief 创建 task 时按值抓取资源，拥有 buffer 的初始化范围只在完成后发布。
   */
  static auto read_owned(std::shared_ptr<io::detail::resource_state> lease,
                         io::io_buffer buffer)
      -> task<expected<io::io_transfer>> {
    auto result =
        co_await io::read(std::move(lease), buffer.data(), buffer.capacity(),
                          std::numeric_limits<std::uint64_t>::max())
            .empty_success();
    if (!result)
      co_return std::unexpected{result.error()};
    buffer.set_size(*result);
    co_return io::io_transfer{std::move(buffer), *result};
  }
  static auto read_exact_impl(std::shared_ptr<io::detail::resource_state> lease,
                              std::span<char> buffer) -> task<expected<void>> {
    if (buffer.empty())
      co_return expected<void>{};
    char owner_identity{};
    auto reservation = io::detail::reserve_direction(
        lease, io::Interest::readable, &owner_identity);
    if (!reservation)
      co_return std::unexpected{reservation.error()};
    std::size_t completed{};
    while (!buffer.empty()) {
      auto result = co_await io::read(lease, buffer.data(), buffer.size(),
                                      std::numeric_limits<std::uint64_t>::max())
                        .reservation(&owner_identity)
                        .empty_success();
      if (!result)
        co_return std::unexpected{Error{result.error().value(),
                                        completed + result.error().progress(),
                                        result.error().domain()}};
      if (!*result)
        co_return std::unexpected{
            Error{Error::UnexpectedEOF, completed, error_domain::faio}};
      completed += *result;
      buffer = buffer.subspan(*result);
      co_await this_coro::yield_if_needed();
    }
    co_return expected<void>{};
  }
  detail::Socket socket_;
};
/** @brief 创建内核匿名管道，返回发送端与接收端，不创建额外线程。 */
[[nodiscard]] inline auto
pair(io::io_context context = io::io_context::current())
    -> expected<std::pair<Sender, Receiver>> {
  int descriptors[2];
#if defined(__linux__)
  if (::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) < 0)
    return std::unexpected{make_error(errno)};
#else
  if (::pipe(descriptors) < 0)
    return std::unexpected{make_error(errno)};
#endif
  owned_native_fd read_end{descriptors[0]}, write_end{descriptors[1]};
  auto reader = Receiver::from_native(context, std::move(read_end));
  if (!reader)
    return std::unexpected{reader.error()};
  auto writer = Sender::from_native(std::move(context), std::move(write_end));
  if (!writer)
    return std::unexpected{writer.error()};
  return std::pair{std::move(*writer), std::move(*reader)};
}
} // namespace faio::net::unix::pipe
#endif
