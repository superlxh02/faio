/**
 * @file faio/detail/io/platform/unix/request_operations.hpp
 * @brief 跨平台异步 IO：统一请求系统调用实现。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#include "faio/detail/io/core/request_rules.hpp"
#include "faio/detail/io/core/resource_state.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
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
/// @brief 校验借用句柄属性，并为就绪后端准备必要的非阻塞设置。
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

inline int close_descriptor(int fd) noexcept {
  return ::close(fd);
}

/// @brief 请求安全写半关闭，不隐含关闭读方向或伪造读 EOF。
inline void shutdown_write(int fd) noexcept {
  (void)::shutdown(fd, SHUT_WR);
}

inline std::uint32_t readiness_flags(std::int64_t result) noexcept {
  std::uint32_t flags{};
  if (result & (POLLIN | POLLPRI))
    flags |= readable_bit;
  if (result & POLLOUT)
    flags |= writable_bit;
  if (result & POLLERR)
    flags |= error_bit;
  /// @brief 原生全 HUP 与 reactor 保持双方向终态提示。
  if (result & POLLHUP)
    flags |=
        readable_bit | writable_bit | closed_bit;
#ifdef POLLRDHUP
  /// @brief 原生读半关闭不能冒充写方向关闭。
  if (result & POLLRDHUP)
    flags |= readable_bit | read_closed_bit;
#endif
  return flags;
}

/// @brief 在资源首次绑定控制面查询 socket 地址族并缓存结果。
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

/// @brief 根据请求种类及资源属性判定是否需要文件执行路径。
inline bool is_file_request(const io_request& req) noexcept {
  return req.kind == operation_kind::open || req.kind == operation_kind::open2
         || req.kind == operation_kind::fsync
         || ((req.kind == operation_kind::read || req.kind == operation_kind::write
              || req.kind == operation_kind::readv || req.kind == operation_kind::writev)
             && req.resource && req.resource->regular_file);
}

/// @brief 检查原生关闭是否可能阻塞，决定是否交给隔离清理服务。
inline bool needs_deferred_close(const resource_state& resource, int fd) noexcept {
  /// @brief 普通文件不能套用 socket 或管道的 readiness 执行路径。
  if (resource.regular_file)
    return true;
  ::linger option{};
  socklen_t length = sizeof(option);
  return ::getsockopt(fd, SOL_SOCKET, SO_LINGER, &option, &length) == 0 && option.l_onoff
         && option.l_linger > 0;
}

template <class F>
inline ssize_t pipe_write(F&& call) noexcept {
  sigset_t signal_set, previous, pending;
  sigemptyset(&signal_set);
  sigaddset(&signal_set, SIGPIPE);
  /// @brief 保存原线程掩码。
  const int mask_error = ::pthread_sigmask(SIG_BLOCK, &signal_set,
                                           &previous);
  if (mask_error) {
    errno = mask_error;
    return -1;
  }
  (void)::sigpending(&pending);
  /// @brief 保留外部已有信号。
  const bool already_pending = sigismember(&pending, SIGPIPE) == 1;
  /// @brief 只在信号受保护的区间执行本次非阻塞 write/writev。
  const auto result = call();
  /// @brief 后续信号清理 syscall 不能覆盖本次 IO 的 errno。
  const int error = errno;
  if (result < 0 && error == EPIPE && !already_pending) {
    (void)::sigpending(&pending);
    if (sigismember(&pending, SIGPIPE) == 1) {
      int consumed{};
      (void)::sigwait(&signal_set, &consumed);
    }
  }
  /// @brief 无论成功失败都恢复调用者的掩码。
  (void)::pthread_sigmask(SIG_SETMASK, &previous,
                          nullptr);
  errno = error;
  return result;
}

/// @brief 执行统一 RECV 或 SEND 非阻塞系统调用并立即捕获错误。
template <class Request>
inline std::int64_t perform_socket_scalar(Request& request) noexcept {
  /// @brief 紧凑描述不能解释为含地址/消息/文件输入的其他 opcode。
  if (request.kind != operation_kind::recv && request.kind != operation_kind::send)
    return -EINVAL;
  /// @brief 与完整流空请求保持相同返回语义。
  if (request.empty_success && request.length == 0)
    return request.fd >= 0 ? 0 : -EBADF;
  ssize_t result;
  do {
    if (request.kind == operation_kind::recv) {
      result = ::recv(request.fd, request.buffer, request.length, request.flags);
    } else {
      /// @brief Linux 仍原样避免 SIGPIPE；Darwin 仍由 socket 的 SO_NOSIGPIPE 保护。
      result = ::send(request.fd,
                      request.const_buffer,
                      request.length,
                      request.flags
#ifdef MSG_NOSIGNAL
                          | MSG_NOSIGNAL
#endif
      );

    }
  /// @brief 未传输 EINTR 不创建另一代请求。
  } while (result < 0 && errno == EINTR);
  /// @brief 短读/短写保留实际结果，EAGAIN 交回公共状态机。
  return result >= 0 ? result : -errno;
}

/// @brief 按统一请求种类执行真实系统调用，保留短读写进度与原生错误。
inline std::int64_t perform(io_request& r, bool& connecting) noexcept {
  /// @brief sentinel 外定位偏移必须可以表示为 POSIX off_t。
  if ((r.kind == operation_kind::read || r.kind == operation_kind::write
       || r.kind == operation_kind::readv || r.kind == operation_kind::writev)
      && r.offset != UINT64_MAX && r.resource && r.resource->regular_file
      && r.offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
    return -EINVAL;
  /// @brief 空读写不等 readiness，但不能把失效描述符报告为成功。
  if (empty_stream_request(r))
    return r.fd >= 0 ? 0 : -EBADF;

  ssize_t result = -1;
  do {
    switch (r.kind) {
      case operation_kind::recv:
      case operation_kind::send:
        /// @brief 完整/紧凑请求共用同一 syscall 语义。
        return perform_socket_scalar(r);
      case operation_kind::recvfrom:
        result = ::recvfrom(
            r.fd, r.buffer, r.length, r.flags, r.output_address, r.output_address_length);
        break;
      case operation_kind::sendto:
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
      /// @brief recvmsg 直接填写协程租约覆盖的 name/control/iovec 输出。
      case operation_kind::recvmsg:

        result = ::recvmsg(r.fd, r.output_message, r.flags);
        break;
      /// @brief msghdr/iovec 描述符由请求拥有，数据本体保持外部租约。
      case operation_kind::sendmsg:

        result = ::sendmsg(r.fd,
                           &r.message,
                           r.flags
#ifdef MSG_NOSIGNAL
                               | MSG_NOSIGNAL
#endif
        );
        break;
      /// @brief 流忽略旧 raw API 的 offset；只有普通文件使用定位 IO。
      case operation_kind::read:

        result = (r.offset == UINT64_MAX || (r.resource && !r.resource->regular_file))
                     ? ::read(r.fd, r.buffer, r.length)
                     : ::pread(r.fd, r.buffer, r.length, static_cast<off_t>(r.offset));
        break;
      case operation_kind::write:
        result = (r.offset == UINT64_MAX || (r.resource && !r.resource->regular_file))
                     ? (r.resource && !r.resource->regular_file
                            ? pipe_write([&] { return ::write(r.fd, r.const_buffer, r.length); })
                            : ::write(r.fd, r.const_buffer, r.length))
                     : ::pwrite(r.fd, r.const_buffer, r.length, static_cast<off_t>(r.offset));
        break;
      case operation_kind::readv:
        if (r.flags) {
#if defined(__linux__) && defined(__GLIBC__)
          /// @brief RWF_* 由内核实际执行；流以 offset=-1 使用当前位置，不能静默忽略 flags。
          const auto offset = r.offset == UINT64_MAX || (r.resource && !r.resource->regular_file)
                                  ? off_t{-1}
                                  : static_cast<off_t>(r.offset);
          result = ::preadv2(
              r.fd, r.vectors.data(), static_cast<int>(r.vectors.size()), offset, r.flags);
          break;
#else
          /// @brief 本平台没有等价 RWF 接口，明确拒绝不支持的扩展。
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
      case operation_kind::writev:
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
      /// @brief 首次 connect 与 readiness 后 SO_ERROR 检查分成两阶段。
      case operation_kind::connect:

        if (!connecting) {
          result = ::connect(r.fd, reinterpret_cast<const sockaddr*>(&r.address), r.address_length);
          if (result < 0 && errno == EINPROGRESS) {
            /// @brief 不再次 connect；后续等待可写再检查连接结果。
            connecting = true;
            return -EAGAIN;
          }
        } else {
          /// @brief 保存准备阶段错误码，准备失败表示域尚未接管异步完成责任。
          int error{};
          socklen_t size = sizeof(error);
          if (::getsockopt(r.fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
            return -errno;
          if (error)
            return -error;
          /// @brief SO_ERROR=0 未必已连接，getpeername 排除提前观察。
          sockaddr_storage peer{};
          socklen_t peer_length = sizeof(peer);
          if (::getpeername(r.fd, reinterpret_cast<sockaddr*>(&peer), &peer_length) < 0)
            return errno == ENOTCONN ? -EAGAIN : -errno;
          result = 0;
        }
        break;
      /// @brief 新 socket 在对外发布前完成 NONBLOCK/CLOEXEC 设置。
      case operation_kind::accept:

#if defined(__linux__)
        result = ::accept4(r.fd,
                           r.output_address,
                           r.output_address_length,
                           r.flags | SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        /// @brief Darwin accept 无 flags 参数，不能吞掉 Linux 专属扩展。
        if (r.flags)
          return -ENOTSUP;

        result = ::accept(r.fd, r.output_address, r.output_address_length);
        if (result >= 0) {
          const int fd = static_cast<int>(result);
          const int flags = ::fcntl(fd, F_GETFL, 0);
          if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
              || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            /// @brief 后续信号清理 syscall 不能覆盖本次 IO 的 errno。
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
      /// @brief 文件打开在隔离 lane 中执行，路径字符串归请求拥有。
      case operation_kind::open:

        result =
            ::openat(r.fd, r.path.c_str(), r.flags | O_CLOEXEC, static_cast<mode_t>(r.argument));
        break;
      case operation_kind::open2:
#if defined(__linux__) && defined(SYS_openat2)
      {
        const open_how how{r.extension_flags | O_CLOEXEC, r.extension_mode, r.extension_resolve};
        result = ::syscall(SYS_openat2, r.fd, r.path.c_str(), &how, sizeof(how));
      } break;
#else
        return -ENOTSUP;
#endif
      case operation_kind::fsync:
        if (r.flags & ~1)
          return -EINVAL;
#if defined(__linux__)
        result = r.flags ? ::fdatasync(r.fd) : ::fsync(r.fd);
#else
        result = ::fsync(r.fd);
#endif
        break;
      case operation_kind::shutdown:
        result = ::shutdown(r.fd, r.argument);
        break;
      case operation_kind::socket:
        /// @brief uring 专属提交 flags 没有 POSIX socket 的等价接口。
        if (r.flags)
          return -ENOTSUP;

#if defined(__linux__)
        result = ::socket(r.argument, r.argument2 | SOCK_NONBLOCK | SOCK_CLOEXEC, r.argument3);
#else
        result = ::socket(r.argument, r.argument2, r.argument3);
        if (result >= 0) {
          const int fd = static_cast<int>(result);
          if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            /// @brief 后续信号清理 syscall 不能覆盖本次 IO 的 errno。
            const int error = errno;
            (void)::close(fd);
            return -error;
          }
        }
#endif
        break;
      case operation_kind::getsockopt: {
        auto size = static_cast<socklen_t>(r.length);
        result = ::getsockopt(r.fd, r.argument, r.argument2, r.buffer, &size);
        /// @brief 仅成功登记后才允许发布 registered 标志。
        if (!result)
          result = size;
        break;
      }
      case operation_kind::setsockopt:
        result = ::setsockopt(
            r.fd, r.argument, r.argument2, r.const_buffer, static_cast<socklen_t>(r.length));
        break;
      case operation_kind::socket_inq: {
        int queued{};
        result = ::ioctl(r.fd, FIONREAD, &queued);
        /// @brief 仅成功登记后才允许发布 registered 标志。
        if (!result)
          result = queued;
        break;
      }
      case operation_kind::socket_outq: {
        int queued{};
#if defined(TIOCOUTQ)
        result = ::ioctl(r.fd, TIOCOUTQ, &queued);
#elif defined(SO_NWRITE)
        socklen_t size = sizeof(queued);
        result = ::getsockopt(r.fd, SOL_SOCKET, SO_NWRITE, &queued, &size);
#else
        return -ENOTSUP;
#endif
        /// @brief 仅成功登记后才允许发布 registered 标志。
        if (!result)
          result = queued;
        break;
      }
      case operation_kind::unsupported:
      case operation_kind::send_zc:
      case operation_kind::sendmsg_zc:
        return -ENOTSUP;
      default:
        return -EINVAL;
    }
  /// @brief EINTR 无传输结果，可原样重试本次 syscall。
  } while (result < 0 && errno == EINTR);
  return result >= 0 ? result : -errno;
}
}
