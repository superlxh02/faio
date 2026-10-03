#pragma once
#include "faio/detail/io/awaiter/accept.hpp"
#include "faio/detail/io/awaiter/cancel.hpp"
#include "faio/detail/io/awaiter/close.hpp"
#include "faio/detail/io/awaiter/cmd_sock.hpp"
#include "faio/detail/io/awaiter/connect.hpp"
#include "faio/detail/io/awaiter/fsync.hpp"
#include "faio/detail/io/awaiter/open.hpp"
#include "faio/detail/io/awaiter/read.hpp"
#include "faio/detail/io/awaiter/readv.hpp"
#include "faio/detail/io/awaiter/recv.hpp"
#include "faio/detail/io/awaiter/recvfrom.hpp"
#include "faio/detail/io/awaiter/recvmsg.hpp"
#include "faio/detail/io/awaiter/send.hpp"
#include "faio/detail/io/awaiter/sendmsg.hpp"
#include "faio/detail/io/awaiter/sendto.hpp"
#include "faio/detail/io/awaiter/shutdown.hpp"
#include "faio/detail/io/awaiter/socket.hpp"
#include "faio/detail/io/awaiter/write.hpp"
#include "faio/detail/io/awaiter/writev.hpp"
#include "faio/detail/io/engine.hpp"
#include "faio/detail/io/native_file_op.hpp"
namespace faio::io::detail {
/** @brief 资源包装所有者；移动/共享不改变 IO 归属，析构只请求非阻塞关闭。 */
class FileDescriptor {
protected:
  explicit FileDescriptor(int fd, io_context context = io_context::current(),
                          bool regular = false)
      : resource_(adopt_resource(context, fd, true, regular)) {
    resource_->wrappers.fetch_add(1, std::memory_order_relaxed);
  }
  explicit FileDescriptor(io_context context, int fd, bool regular = false)
      : FileDescriptor(fd, std::move(context), regular) {}
  explicit FileDescriptor(resource_ptr resource)
      : resource_(std::move(resource)) {
    if (resource_)
      resource_->wrappers.fetch_add(1, std::memory_order_relaxed);
  }
  ~FileDescriptor() { release(); }
  FileDescriptor(FileDescriptor &&other) noexcept
      : resource_(std::move(other.resource_)) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      release();
      resource_ = std::move(other.resource_);
    }
    return *this;
  }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;

public:
  int fd() const noexcept { return resource_ ? resource_->fd() : -1; }
  const resource_ptr &resource() const noexcept { return resource_; }
  io_context context() const noexcept {
    return resource_ ? io_context{resource_->owner} : io_context{};
  }
  auto close() noexcept { return Close{resource_}; }
  expected<int> into_native() noexcept {
    if (!resource_)
      return std::unexpected{make_error(EBADF)};
    if (resource_->owner)
      return resource_->owner->detach(*resource_);
    const int value = resource_->handle.exchange(-1);
    resource_->owns_handle = false;
    if (value < 0)
      return std::unexpected{make_error(EBADF)};
    return value;
  }
  int take_fd() noexcept {
    auto value = into_native();
    return value ? *value : -1;
  }
  /** @brief 设置异步句柄为非阻塞；关闭非阻塞模式会返回 EINVAL。
   * @details 后端直接尝试系统调用，O_NONBLOCK 是保障 worker
   * 不被内核阻塞的前提。 如需原生阻塞句柄，应先通过 into_native
   * 导出所有权再修改 flags。
   */
  expected<void> set_nonblocking(bool enabled) const noexcept {
    return with_resource(resource_, Interest::none, [&]() -> expected<void> {
      if (!enabled)
        return std::unexpected{make_error(EINVAL)};
      const int status = ::fcntl(fd(), F_GETFL, 0);
      if (status < 0 || ::fcntl(fd(), F_SETFL, status | O_NONBLOCK) < 0)
        return std::unexpected{make_error(errno)};
      return {};
    });
  }
  expected<bool> nonblocking() const noexcept {
    return with_resource(resource_, Interest::none, [&]() -> expected<bool> {
      const int status = ::fcntl(fd(), F_GETFL, 0);
      if (status < 0)
        return std::unexpected{make_error(errno)};
      return (status & O_NONBLOCK) != 0;
    });
  }

private:
  void release() noexcept {
    if (!resource_)
      return;
    if (resource_->wrappers.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        resource_->owner)
      resource_->owner->request_close(resource_);
    resource_.reset();
  }
  resource_ptr resource_;
};
/** @brief 多观察者 ready awaiter 不消费数据；关闭/取消与读写遵循同一终态协议。
 */
class ReadyAwaiter : public IORegistrantAwaiter<ReadyAwaiter> {
public:
  ReadyAwaiter(resource_ptr resource, Interest interest)
      : IORegistrantAwaiter{[&] {
          auto r = make_request(resource, operation_kind::ready);
          r.argument = static_cast<int>(interest);
          return r;
        }()} {}
  ReadyAwaiter(int fd, Interest interest)
      : IORegistrantAwaiter{[&] {
          io_request r;
          r.fd = fd;
          r.kind = operation_kind::ready;
          r.argument = static_cast<int>(interest);
          return r;
        }()} {}
  expected<Ready> await_resume() const noexcept {
    if (_user_data.result < 0)
      return std::unexpected{make_error(static_cast<int>(-_user_data.result))};
    return Ready{static_cast<std::uint32_t>(_user_data.result),
                 _user_data.transferred};
  }
};
} // namespace faio::io::detail
namespace faio::io {
// 兼容 raw fd 及现代稳定资源重载；平台类型不透过 liburing 进入公共头。
template <class F>
inline auto accept(F fd, sockaddr *address, socklen_t *length, int flags = 0) {
  return detail::Accept{std::move(fd), address, length, flags};
}
template <class F> inline auto cancel(F fd, unsigned flags = 0) {
  return detail::Cancel{std::move(fd), flags};
}
template <class F> inline auto close(F fd) {
  return detail::Close{std::move(fd)};
}
template <class F>
inline auto connect(F fd, const sockaddr *address, socklen_t length) {
  return detail::Connect{std::move(fd), address, length};
}
template <class F> inline auto fsync(F fd, unsigned flags = 0) {
  return detail::Fsync{std::move(fd), flags};
}
inline auto open(const char *path, int flags, mode_t mode = 0666) {
  return detail::Open{path, flags, mode};
}
inline auto openat(int directory, const char *path, int flags,
                   mode_t mode = 0666) {
  return detail::Open{directory, path, flags, mode};
}
template <class F>
inline auto read(F fd, void *buffer, std::size_t length,
                 std::uint64_t offset = UINT64_MAX) {
  return detail::Read{std::move(fd), buffer, length, offset};
}
template <class F>
inline auto write(F fd, const void *buffer, std::size_t length,
                  std::uint64_t offset = UINT64_MAX) {
  return detail::Write{std::move(fd), buffer, length, offset};
}
template <class F>
inline auto readv(F fd, const iovec *vectors, unsigned count,
                  std::uint64_t offset = UINT64_MAX, int flags = 0) {
  return detail::ReadV{std::move(fd), vectors, count, offset, flags};
}
template <class F>
inline auto writev(F fd, const iovec *vectors, unsigned count,
                   std::uint64_t offset = UINT64_MAX, int flags = 0) {
  return detail::WriteV{std::move(fd), vectors, count, offset, flags};
}
template <class F>
inline auto recv(F fd, void *buffer, std::size_t length, int flags = 0) {
  return detail::Recv{std::move(fd), buffer, length, flags};
}
template <class F>
inline auto send(F fd, const void *buffer, std::size_t length, int flags = 0) {
  return detail::Send{std::move(fd), buffer, length, flags};
}
template <class F>
inline auto recvfrom(F fd, void *buffer, std::size_t length, int flags,
                     sockaddr *address, socklen_t *address_length) {
  return detail::RecvFrom{std::move(fd), buffer,  length,
                          flags,         address, address_length};
}
template <class F>
inline auto sendto(F fd, const void *buffer, std::size_t length, int flags,
                   const sockaddr *address, socklen_t address_length) {
  return detail::SendTo{std::move(fd), buffer,  length,
                        flags,         address, address_length};
}
template <class F>
inline auto recvmsg(F fd, msghdr *message, unsigned flags = 0) {
  return detail::RecvMsg{std::move(fd), message, flags};
}
template <class F>
inline auto sendmsg(F fd, const msghdr *message, unsigned flags = 0) {
  return detail::SendMsg{std::move(fd), message, flags};
}
template <class F> inline auto shutdown(F fd, int how) {
  return detail::Shutdown{std::move(fd), how};
}
inline auto socket(int family, int type, int protocol, unsigned flags = 0) {
  return detail::Socket{family, type, protocol, flags};
}
template <class F> inline auto ready(F fd, Interest interest) {
  return detail::ReadyAwaiter{std::move(fd), interest};
}
inline auto getsockopt(int fd, int level, int option, void *value, int length) {
  return detail::CmdSock{
      detail::socket_getsockopt_command, fd, level, option, value, length};
}
inline auto setsockopt(int fd, int level, int option, void *value, int length) {
  return detail::CmdSock{
      detail::socket_setsockopt_command, fd, level, option, value, length};
}
inline auto cmdsock(int command, int fd, int level, int option, void *value,
                    int length) {
  return detail::CmdSock{command, fd, level, option, value, length};
}
template <class F>
inline auto send_zc(F fd, const void *buffer, std::size_t length, int flags,
                    unsigned zc_flags = 0) {
  return detail::SendZC{std::move(fd), buffer, length, flags, zc_flags};
}
template <class F>
inline auto sendmsg_zc(F fd, const msghdr *message, unsigned flags = 0) {
  return detail::SendMsgZC{std::move(fd), message, flags};
}
} // namespace faio::io

#if defined(__linux__)
namespace faio::io {
/** @brief Linux 特有路径解析约束扩展；旧内核返回 ENOSYS，不伪装支持 resolve
 * flags。 */
inline auto open2(const char *path, const open_how *how) {
  return detail::Open2{path, how};
}
inline auto openat2(int directory, const char *path, const open_how *how) {
  return detail::Open2{directory, path, how};
}
} // namespace faio::io
#endif
