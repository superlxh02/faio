/**
 * @file faio/detail/io/platform/unix/request_operations.hpp
 * @brief 统一请求系统调用实现。
 */
#pragma once
#include "faio/detail/io/core/request_rules.hpp"
#include "faio/detail/io/core/resource_state.hpp"
#include <cerrno>
#include <limits>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <system_error>
#include "faio/detail/io/protocol/reactor.hpp"
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif

namespace faio::io::detail::unix_operations {
/** @brief 校验借用句柄属性，并为就绪后端准备必要的非阻塞设置。 */
inline bool prepare_borrowed_descriptor(int fd) {
  struct stat status{};
  if (::fstat(fd, &status) < 0)
    throw std::system_error(errno, std::generic_category());
  const bool regular =
      S_ISREG(status.st_mode) || S_ISDIR(status.st_mode) || S_ISBLK(status.st_mode);
  if (!regular) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
      throw std::system_error(errno, std::generic_category());
#ifdef F_SETNOSIGPIPE
    if (S_ISFIFO(status.st_mode) && ::fcntl(fd, F_SETNOSIGPIPE, 1) < 0)
      throw std::system_error(errno, std::generic_category());
#endif
#ifdef SO_NOSIGPIPE
    if (S_ISSOCK(status.st_mode)) {
      const int enabled = 1;
      if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
        throw std::system_error(errno, std::generic_category());
    }
#endif
  }
  return regular;
}

/** @brief 关闭拥有的原生描述符。 */
inline int close_descriptor(int fd) noexcept {
  return ::close(fd);
}

/** @brief 请求安全写半关闭，保持读方向独立，不产生额外的读 EOF。 */
inline void shutdown_write(int fd) noexcept {
  (void)::shutdown(fd, SHUT_WR);
}

/** @brief 读取资源的就绪标志。 */
inline std::uint32_t readiness_flags(std::int64_t result) noexcept {
  std::uint32_t flags{};
  if (result & (POLLIN | POLLPRI))
    flags |= READABLE_BIT;
  if (result & POLLOUT)
    flags |= WRITABLE_BIT;
  if (result & POLLERR)
    flags |= ERROR_BIT;
  if (result & POLLHUP)
    flags |= READABLE_BIT | WRITABLE_BIT | CLOSED_BIT;
#ifdef POLLRDHUP
  if (result & POLLRDHUP)
    flags |= READABLE_BIT | READ_CLOSED_BIT;
#endif
  return flags;
}

/** @brief 在控制面首次绑定资源时查询 socket 地址族并缓存结果。 */
inline int query_socket_family(int fd) noexcept {
  int family = AF_UNSPEC;
#if defined(SO_DOMAIN)
  socklen_t length = sizeof(family);
  if (::getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &family, &length) < 0)
    return AF_UNSPEC;
#else
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0)
    return AF_UNSPEC;
  family = address.ss_family;
#endif
  return family;
}

/** @brief 根据请求种类及资源属性判定是否需要文件执行路径。 */
inline bool is_file_request(const io_request& req) noexcept {
  return req.kind == operation_kind::OPEN || req.kind == operation_kind::OPEN2
         || req.kind == operation_kind::FSYNC
         || ((req.kind == operation_kind::READ || req.kind == operation_kind::WRITE
              || req.kind == operation_kind::READV || req.kind == operation_kind::WRITEV)
             && req.resource && req.resource->regular_file);
}

/** @brief 检查原生关闭是否可能阻塞，决定是否交给隔离清理服务。 */
inline bool needs_deferred_close(const resource_state& resource, int fd) noexcept {
  if (resource.regular_file)
    return true;
  ::linger option{};
  socklen_t length = sizeof(option);
  return ::getsockopt(fd, SOL_SOCKET, SO_LINGER, &option, &length) == 0 && option.l_onoff
         && option.l_linger > 0;
}

/** @brief 向管道写入并处理 SIGPIPE。 */
template <class F>
inline ssize_t pipe_write(F&& call) noexcept {
  sigset_t signal_set, previous, pending;
  sigemptyset(&signal_set);
  sigaddset(&signal_set, SIGPIPE);
  const int mask_error = ::pthread_sigmask(SIG_BLOCK, &signal_set, &previous);
  if (mask_error) {
    errno = mask_error;
    return -1;
  }
  (void)::sigpending(&pending);
  const bool already_pending = sigismember(&pending, SIGPIPE) == 1;
  const auto result = call();
  const int error = errno;
  if (result < 0 && error == EPIPE && !already_pending) {
    (void)::sigpending(&pending);
    if (sigismember(&pending, SIGPIPE) == 1) {
      int consumed{};
      (void)::sigwait(&signal_set, &consumed);
    }
  }
  (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  errno = error;
  return result;
}

/** @brief 执行统一 RECV 或 SEND 非阻塞系统调用并立即捕获错误。 */
template <class Request>
inline std::int64_t perform_socket_scalar(Request& request) noexcept {
  if (request.kind != operation_kind::RECV && request.kind != operation_kind::SEND)
    return -EINVAL;
  if (request.empty_success && request.length == 0)
    return request.fd >= 0 ? 0 : -EBADF;
  ssize_t result;
  do {
    if (request.kind == operation_kind::RECV) {
      result = ::recv(request.fd, request.buffer, request.length, request.flags);
    } else {
      result = ::send(request.fd,
                      request.const_buffer,
                      request.length,
                      request.flags
#ifdef MSG_NOSIGNAL
                          | MSG_NOSIGNAL
#endif
      );
    }
  } while (result < 0 && errno == EINTR);
  return result >= 0 ? result : -errno;
}

/** @brief 按统一请求种类执行真实系统调用，保留短读写进度与原生错误。 */
inline std::int64_t perform(io_request& r, bool& connecting) noexcept {
  if ((r.kind == operation_kind::READ || r.kind == operation_kind::WRITE
       || r.kind == operation_kind::READV || r.kind == operation_kind::WRITEV)
      && r.offset != UINT64_MAX && r.resource && r.resource->regular_file
      && r.offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
    return -EINVAL;
  if (empty_stream_request(r))
    return r.fd >= 0 ? 0 : -EBADF;

  ssize_t result = -1;
  do {
    switch (r.kind) {
      case operation_kind::RECV:
      case operation_kind::SEND:
        return perform_socket_scalar(r);
      case operation_kind::RECVFROM:
        result = ::recvfrom(
            r.fd, r.buffer, r.length, r.flags, r.output_address, r.output_address_length);
        break;
      case operation_kind::SENDTO:
        result = ::sendto(r.fd,
                          r.const_buffer,
                          r.length,
                          r.flags
#ifdef MSG_NOSIGNAL
                              | MSG_NOSIGNAL
#endif
                          ,
                          reinterpret_cast<const sockaddr*>(&r.address),
                          r.address_length);
        break;
      case operation_kind::RECVMSG:

        result = ::recvmsg(r.fd, r.output_message, r.flags);
        break;
      case operation_kind::SENDMSG:

        result = ::sendmsg(r.fd,
                           &r.message,
                           r.flags
#ifdef MSG_NOSIGNAL
                               | MSG_NOSIGNAL
#endif
        );
        break;
      case operation_kind::READ:

        result = (r.offset == UINT64_MAX || (r.resource && !r.resource->regular_file))
                     ? ::read(r.fd, r.buffer, r.length)
                     : ::pread(r.fd, r.buffer, r.length, static_cast<off_t>(r.offset));
        break;
      case operation_kind::WRITE:
        result = (r.offset == UINT64_MAX || (r.resource && !r.resource->regular_file))
                     ? (r.resource && !r.resource->regular_file
                            ? pipe_write([&] { return ::write(r.fd, r.const_buffer, r.length); })
                            : ::write(r.fd, r.const_buffer, r.length))
                     : ::pwrite(r.fd, r.const_buffer, r.length, static_cast<off_t>(r.offset));
        break;
      case operation_kind::READV:
        if (r.flags) {
#if defined(__linux__) && defined(__GLIBC__)
          const auto offset = r.offset == UINT64_MAX || (r.resource && !r.resource->regular_file)
                                  ? off_t{-1}
                                  : static_cast<off_t>(r.offset);
          result = ::preadv2(
              r.fd, r.vectors.data(), static_cast<int>(r.vectors.size()), offset, r.flags);
          break;
#else
          return -ENOTSUP;
#endif
        }
        result = (r.offset == UINT64_MAX || (r.resource && !r.resource->regular_file))
                     ? ::readv(r.fd, r.vectors.data(), static_cast<int>(r.vectors.size()))
                     : ::preadv(r.fd,
                                r.vectors.data(),
                                static_cast<int>(r.vectors.size()),
                                static_cast<off_t>(r.offset));
        break;
      case operation_kind::WRITEV:
        if (r.flags) {
#if defined(__linux__) && defined(__GLIBC__)
          const auto offset = r.offset == UINT64_MAX || (r.resource && !r.resource->regular_file)
                                  ? off_t{-1}
                                  : static_cast<off_t>(r.offset);
          if (r.resource && !r.resource->regular_file)
            result = pipe_write([&] {
              return ::pwritev2(
                  r.fd, r.vectors.data(), static_cast<int>(r.vectors.size()), offset, r.flags);
            });
          else
            result = ::pwritev2(
                r.fd, r.vectors.data(), static_cast<int>(r.vectors.size()), offset, r.flags);
          break;
#else
          return -ENOTSUP;
#endif
        }
        result = (r.offset == UINT64_MAX || (r.resource && !r.resource->regular_file))
                     ? (r.resource && !r.resource->regular_file
                            ? pipe_write([&] {
                                return ::writev(
                                    r.fd, r.vectors.data(), static_cast<int>(r.vectors.size()));
                              })
                            : ::writev(r.fd, r.vectors.data(), static_cast<int>(r.vectors.size())))
                     : ::pwritev(r.fd,
                                 r.vectors.data(),
                                 static_cast<int>(r.vectors.size()),
                                 static_cast<off_t>(r.offset));
        break;
      case operation_kind::CONNECT:

        if (!connecting) {
          result = ::connect(r.fd, reinterpret_cast<const sockaddr*>(&r.address), r.address_length);
          if (result < 0 && errno == EINPROGRESS) {
            connecting = true;
            return -EAGAIN;
          }
        } else {
          int error{};
          socklen_t size = sizeof(error);
          if (::getsockopt(r.fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
            return -errno;
          if (error)
            return -error;
          sockaddr_storage peer{};
          socklen_t peer_length = sizeof(peer);
          if (::getpeername(r.fd, reinterpret_cast<sockaddr*>(&peer), &peer_length) < 0)
            return errno == ENOTCONN ? -EAGAIN : -errno;
          result = 0;
        }
        break;
      case operation_kind::ACCEPT:

#if defined(__linux__)
        result = ::accept4(r.fd,
                           r.output_address,
                           r.output_address_length,
                           r.flags | SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        if (r.flags)
          return -ENOTSUP;

        result = ::accept(r.fd, r.output_address, r.output_address_length);
        if (result >= 0) {
          const int fd = static_cast<int>(result);
          const int flags = ::fcntl(fd, F_GETFL, 0);
          if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
              || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            const int error = errno;
            (void)::close(fd);
            return -error;
          }
#ifdef SO_NOSIGPIPE
          const int enabled = 1;
          (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
        }
#endif
        break;
      case operation_kind::OPEN:

        result =
            ::openat(r.fd, r.path.c_str(), r.flags | O_CLOEXEC, static_cast<mode_t>(r.argument));
        break;
      case operation_kind::OPEN2:
#if defined(__linux__) && defined(SYS_openat2)
      {
        const open_how how{r.extension_flags | O_CLOEXEC, r.extension_mode, r.extension_resolve};
        result = ::syscall(SYS_openat2, r.fd, r.path.c_str(), &how, sizeof(how));
      } break;
#else
        return -ENOTSUP;
#endif
      case operation_kind::FSYNC:
        if (r.flags & ~1)
          return -EINVAL;
#if defined(__linux__)
        result = r.flags ? ::fdatasync(r.fd) : ::fsync(r.fd);
#else
        result = ::fsync(r.fd);
#endif
        break;
      case operation_kind::SHUTDOWN:
        result = ::shutdown(r.fd, r.argument);
        break;
      case operation_kind::SOCKET:
        if (r.flags)
          return -ENOTSUP;

#if defined(__linux__)
        result = ::socket(r.argument, r.argument2 | SOCK_NONBLOCK | SOCK_CLOEXEC, r.argument3);
#else
        result = ::socket(r.argument, r.argument2, r.argument3);
        if (result >= 0) {
          const int fd = static_cast<int>(result);
          if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            const int error = errno;
            (void)::close(fd);
            return -error;
          }
        }
#endif
        break;
      case operation_kind::GETSOCKOPT: {
        auto size = static_cast<socklen_t>(r.length);
        result = ::getsockopt(r.fd, r.argument, r.argument2, r.buffer, &size);
        if (!result)
          result = size;
        break;
      }
      case operation_kind::SETSOCKOPT:
        result = ::setsockopt(
            r.fd, r.argument, r.argument2, r.const_buffer, static_cast<socklen_t>(r.length));
        break;
      case operation_kind::SOCKET_INQ: {
        int queued{};
        result = ::ioctl(r.fd, FIONREAD, &queued);
        if (!result)
          result = queued;
        break;
      }
      case operation_kind::SOCKET_OUTQ: {
        int queued{};
#if defined(TIOCOUTQ)
        result = ::ioctl(r.fd, TIOCOUTQ, &queued);
#elif defined(SO_NWRITE)
        socklen_t size = sizeof(queued);
        result = ::getsockopt(r.fd, SOL_SOCKET, SO_NWRITE, &queued, &size);
#else
        return -ENOTSUP;
#endif
        if (!result)
          result = queued;
        break;
      }
      case operation_kind::UNSUPPORTED:
      case operation_kind::SEND_ZC:
      case operation_kind::SENDMSG_ZC:
        return -ENOTSUP;
      default:
        return -EINVAL;
    }
  } while (result < 0 && errno == EINTR);
  return result >= 0 ? result : -errno;
}
}  // namespace faio::io::detail::unix_operations
