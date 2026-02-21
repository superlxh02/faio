#ifndef FAIO_DETAIL_IO_IO_HPP
#define FAIO_DETAIL_IO_IO_HPP

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

namespace faio::io::detail {
class FileDescriptor {
protected:
  explicit FileDescriptor(int fd) : _fd{fd} {}

  ~FileDescriptor() {
    if (_fd >= 0) {
      do_close();
    }
  }

  FileDescriptor(FileDescriptor &&other) noexcept : _fd{other._fd} {
    other._fd = -1;
  }

  auto operator=(FileDescriptor &&other) noexcept -> FileDescriptor & {
    if (_fd >= 0) {
      do_close();
    }
    _fd = other._fd;
    other._fd = -1;
    return *this;
  }

  // Delete copy
  FileDescriptor(const FileDescriptor &other) = delete;
  FileDescriptor &operator=(const FileDescriptor &other) = delete;

public:
  auto close() noexcept {
    auto fd = _fd;
    _fd = -1;
    return Close{fd};
  }

  [[nodiscard]]
  auto fd() const noexcept {
    return _fd;
  }

  [[nodiscard]]
  auto take_fd() noexcept {
    auto ret = _fd;
    _fd = -1;
    return ret;
  }

  [[nodiscard]]
  auto set_nonblocking(bool status) const noexcept -> expected<void> {
    auto flags = ::fcntl(_fd, F_GETFL, 0);
    if (status) {
      flags |= O_NONBLOCK;
    } else {
      flags &= ~O_NONBLOCK;
    }
    if (::fcntl(_fd, F_SETFL, flags) == -1) [[unlikely]] {
      return std::unexpected{make_error(errno)};
    }
    return {};
  }

  [[nodiscard]]
  auto nonblocking() const noexcept -> expected<bool> {
    auto flags = ::fcntl(_fd, F_GETFL, 0);
    if (flags == -1) [[unlikely]] {
      return std::unexpected{make_error(errno)};
    }
    return flags & O_NONBLOCK;
  }

private:
  void do_close() noexcept {
    auto sqe = current_uring->get_sqe();
    if (sqe != nullptr) [[likely]] {
      // async close
      io_uring_prep_close(sqe, _fd);
      io_uring_sqe_set_data(sqe, nullptr);
    } else {
      // sync close
      for (auto i = 1; i <= 3; i += 1) {
        auto ret = ::close(_fd);
        if (ret == 0) [[likely]] {
          break;
        } else {
          fastlog::console.error("close {} failed, error: {}, times: {}", _fd,
                                 strerror(errno), i);
        }
      }
    }
    _fd = -1;
  }

protected:
  int _fd;
};
} // namespace faio::io::detail

namespace faio::io {
// 接受连接
static inline auto accept(int fd, struct sockaddr *addr, socklen_t *addrlen,
                          int flags) {
  return detail::Accept{fd, addr, addrlen, flags};
}

// 取消io操作
static inline auto cancel(int fd, unsigned int flags) {
  return detail::Cancel{fd, flags};
}
// 关闭文件描述符
static inline auto close(int fd) { return detail::Close{fd}; }
// 获取socket选项
static inline auto getsockopt(int fd, int level, int optname, void *optval,
                              int optlen) {
  return detail::CmdSock{
      SOCKET_URING_OP_GETSOCKOPT, fd, level, optname, optval, optlen};
}
// 设置socket选项
static inline auto setsockopt(int fd, int level, int optname, void *optval,
                              int optlen) {
  return detail::CmdSock{
      SOCKET_URING_OP_SETSOCKOPT, fd, level, optname, optval, optlen};
}
// 执行socket命令
static inline auto cmdsock(int cmd_op, int fd, int level, int optname,
                           void *optval, int optlen) {
  return detail::CmdSock{cmd_op, fd, level, optname, optval, optlen};
}
// 连接到远程地址
static inline auto connect(int fd, const struct sockaddr *addr,
                           socklen_t addrlen) {
  return detail::Connect{fd, addr, addrlen};
}
// 同步文件
static inline auto fsync(int fd, unsigned fsync_flags) {
  return detail::Fsync{fd, fsync_flags};
}

// 打开文件
inline auto open(const char *path, int flags, mode_t mode) {
  return detail::Open{path, flags, mode};
}

// 打开文件2
static inline auto open2(const char *path, struct open_how *how) {
  return detail::Open2{path, how};
}

// 打开文件at
static inline auto openat(int dfd, const char *path, int flags, mode_t mode) {
  return detail::Open{dfd, path, flags, mode};
}

// 打开文件at2
static inline auto openat2(int dfd, const char *path, struct open_how *how) {
  return detail::Open2{dfd, path, how};
}
// 读取文件
static inline auto read(int fd, void *buf, std::size_t nbytes,
                        uint64_t offset) {
  return detail::Read{fd, buf, nbytes, offset};
}

// 读取文件v
static inline auto readv(int fd, const struct iovec *iovecs, unsigned nr_vecs,
                         __u64 offset, int flags = 0) {
  return detail::ReadV{fd, iovecs, nr_vecs, offset, flags};
}
// 接收数据
static inline auto recv(int sockfd, void *buf, size_t len, int flags) {
  return detail::Recv{sockfd, buf, len, flags};
}

// 接收数据from
static inline auto recvfrom(int sockfd, void *buf, size_t len, int flags,
                            struct sockaddr *addr, socklen_t *addrlen) {
  return detail::RecvFrom{sockfd, buf, len, flags, addr, addrlen};
}

// 接收消息
static inline auto recvmsg(int fd, struct msghdr *msg, unsigned flags) {
  return detail::RecvMsg{fd, msg, flags};
}

// 发送数据
static inline auto send(int sockfd, const void *buf, size_t len, int flags) {
  return detail::Send{sockfd, buf, len, flags};
}

// 发送数据零拷贝
static inline auto send_zc(int sockfd, const void *buf, size_t len, int flags,
                           unsigned zc_flags) {
  return detail::SendZC{sockfd, buf, len, flags, zc_flags};
}

// 发送消息
static inline auto sendmsg(int fd, const struct msghdr *msg, unsigned flags) {
  return detail::SendMsg{fd, msg, flags};
}

// 发送消息零拷贝
static inline auto sendmsg_zc(int fd, const struct msghdr *msg,
                              unsigned flags) {
  return detail::SendMsgZC{fd, msg, flags};
}
// 发送数据to
static inline auto sendto(int sockfd, const void *buf, size_t len, int flags,
                          const struct sockaddr *addr, socklen_t addrlen) {
  return detail::SendTo{sockfd, buf, len, flags, addr, addrlen};
}
// 关闭连接
static inline auto shutdown(int fd, int how) {
  return detail::Shutdown{fd, how};
}
// 创建socket
static inline auto socket(int domain, int type, int protocol,
                          unsigned int flags) {
  return detail::Socket{domain, type, protocol, flags};
}
// 写入文件
static inline auto write(int fd, const void *buf, unsigned nbytes,
                         __u64 offset) {
  return detail::Write(fd, buf, nbytes, offset);
}
// 写入文件v
static inline auto writev(int fd, const struct iovec *iovecs, unsigned nr_vecs,
                          __u64 offset, int flags = 0) {
  return detail::WriteV{fd, iovecs, nr_vecs, offset, flags};
}
} // namespace faio::io

#endif // FAIO_DETAIL_IO_IO_HPP
