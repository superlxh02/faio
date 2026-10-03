#ifndef FAIO_DETAIL_NET_COMMON_SOCKET_HPP
#define FAIO_DETAIL_NET_COMMON_SOCKET_HPP
#include "faio/detail/common/concepts.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/context.hpp"
#include "faio/detail/io/io.hpp"
#include <fcntl.h>
#include <memory>
#include <new>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace faio::net::detail {
class CreateSocketAwaiter;
/** @brief Linux 通过 MSG_NOSIGNAL，Apple 通过 SO_NOSIGPIPE 避免 SIGPIPE。 */
inline constexpr int no_signal_flags =
#ifdef MSG_NOSIGNAL
    MSG_NOSIGNAL;
#else
    0;
#endif
/** @brief 独占原生 socket 的 RAII 所有者；release 明确转移关闭责任。 */
class owned_native_socket {
public:
  owned_native_socket() noexcept = default;
  explicit owned_native_socket(int fd) noexcept : fd_{fd} {}
  ~owned_native_socket() { reset(); }
  owned_native_socket(const owned_native_socket &) = delete;
  auto operator=(const owned_native_socket &) -> owned_native_socket & = delete;
  owned_native_socket(owned_native_socket &&other) noexcept
      : fd_{other.release()} {}
  auto operator=(owned_native_socket &&other) noexcept
      -> owned_native_socket & {
    if (this != &other) {
      reset();
      fd_ = other.release();
    }
    return *this;
  }
  [[nodiscard]] auto get() const noexcept -> int { return fd_; }
  [[nodiscard]] auto release() noexcept -> int {
    return std::exchange(fd_, -1);
  }
  explicit operator bool() const noexcept { return fd_ >= 0; }
  void reset() noexcept {
    // close 的 EINTR 不能盲目重试，否则可能关闭被复用的描述符。
    if (fd_ >= 0)
      ::close(std::exchange(fd_, -1));
  }

private:
  int fd_{-1};
};

/** @brief 后端中立 socket；控制块绑定创建时的 IO domain，移动不改变归属。 */
class Socket : public io::detail::FileDescriptor {
public:
  explicit Socket(int fd, io::io_context context = io::io_context::current())
      : FileDescriptor{fd, std::move(context)} {}
  explicit Socket(std::shared_ptr<io::detail::resource_state> resource)
      : FileDescriptor{std::move(resource)} {}
  Socket(Socket &&) noexcept = default;
  auto operator=(Socket &&) noexcept -> Socket & = default;
  Socket(const Socket &) = delete;
  auto operator=(const Socket &) -> Socket & = delete;
  ~Socket() = default;
  /** @brief 独立方向半边共享同一控制块，绝不复制原生 handle。 */
  [[nodiscard]] auto share() const -> Socket { return Socket{resource()}; }
  template <class Addr>
    requires is_socket_address<Addr>
  [[nodiscard]] auto bind(const Addr &address) -> expected<void> {
    return io::detail::with_resource(
        resource(), io::Interest::none, [&]() -> expected<void> {
          if (::bind(fd(), address.sockaddr(), address.length()) < 0)
            return std::unexpected{make_error(errno)};
          return {};
        });
  }
  [[nodiscard]] auto listen(int backlog = SOMAXCONN) -> expected<void> {
    if (backlog < 0)
      return std::unexpected{make_error(EINVAL)};
    return io::detail::with_resource(
        resource(), io::Interest::none, [&]() -> expected<void> {
          if (::listen(fd(), backlog) < 0)
            return std::unexpected{make_error(errno)};
          return {};
        });
  }
  auto shutdown(io::ShutdownBehavior how) noexcept {
    return io::shutdown(resource(), static_cast<int>(how));
  }
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    auto result = FileDescriptor::into_native();
    if (!result)
      return std::unexpected{result.error()};
    return owned_native_socket{*result};
  }
  /** @brief 统一设置 nonblocking/CLOEXEC；macOS 不假设支持 socket type 标志。
   */
  [[nodiscard]] static auto prepare(int descriptor) noexcept -> expected<void> {
    const int status = ::fcntl(descriptor, F_GETFL, 0);
    if (status < 0 || ::fcntl(descriptor, F_SETFL, status | O_NONBLOCK) < 0)
      return std::unexpected{make_error(errno)};
    const int descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
    if (descriptor_flags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
      return std::unexpected{make_error(errno)};
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                     sizeof(enabled)) < 0)
      return std::unexpected{make_error(errno)};
#endif
    return {};
  }
  /** @brief 失败不接管原生 fd；将 domain 停止、容量耗尽及注册失败转成
   * expected。
   * @details 调用方保留 native guard，直到本方法成功再
   * release，异常期间不丢失关闭责任。
   */
  [[nodiscard]] static auto adopt_checked(int descriptor,
                                          io::io_context context)
      -> expected<Socket> {
    try {
      return Socket{descriptor, std::move(context)};
    } catch (const std::system_error &error) {
      return std::unexpected{make_error(error.code().value())};
    } catch (const std::bad_alloc &) {
      return std::unexpected{make_error(ENOMEM)};
    } catch (const std::overflow_error &) {
      return std::unexpected{make_error(EOVERFLOW)};
    }
  }
  /** @brief 原生导入时校验家族，阻止把 Unix handle 当作 IP handle 解释。 */
  template <class Addr>
  [[nodiscard]] static auto validate_family(int descriptor) noexcept
      -> expected<void> {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address),
                      &length) < 0)
      return std::unexpected{make_error(errno)};
    const int expected_family = Addr{}.family();
    const bool valid =
        expected_family == AF_UNIX
            ? address.ss_family == AF_UNIX
            : address.ss_family == AF_INET || address.ss_family == AF_INET6;
    if (!valid)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    return {};
  }
  template <class T = Socket>
  [[nodiscard]] static auto
  create(int domain, int type, int protocol,
         io::io_context context = io::io_context::current()) -> expected<T> {
#if defined(__linux__)
    const int descriptor =
        ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
#else
    const int descriptor = ::socket(domain, type, protocol);
#endif
    if (descriptor < 0)
      return std::unexpected{make_error(errno)};
    owned_native_socket guard{descriptor};
    if (auto result = prepare(descriptor); !result)
      return std::unexpected{result.error()};
    // 构造控制块可能分配内存；guard 在异常时仍负责关闭 fd。
    auto socket = adopt_checked(descriptor, std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)guard.release();
    if constexpr (std::same_as<T, Socket>)
      return socket;
    else
      return T{std::move(*socket)};
  }
  /** @brief 供异步连接组合直接创建 socket，已有原生 opcode 使用 SOCKET
   * SQE/CQE。
   * @param context 长期 IO 归属；空 context 在协程首次运行时确定为当前 domain。
   * @details 复用 raw socket awaiter，不先执行 ::socket，不增加 readiness
   * 观察， 也不将原生请求交给应用线程池。同步 create 的 expected 合同保持独立。
   */
  [[nodiscard]] static auto create_async(int domain, int type, int protocol,
                                         io::io_context context)
      -> CreateSocketAwaiter;
};
/** @brief 原生 socket 创建结果的拥有权转换，不分配额外 task 协程帧。
 * @details 挂起与恢复直接委托现有 raw awaiter；本类型仅在成功完成后导入 fd。
 */
class CreateSocketAwaiter {
public:
  CreateSocketAwaiter(int domain, int type, int protocol,
                      io::io_context context)
      : context_{context ? std::move(context) : io::io_context::current()},
        operation_{io::socket(domain, type, protocol).with_context(context_)} {}
  CreateSocketAwaiter(const CreateSocketAwaiter &) = delete;
  CreateSocketAwaiter(CreateSocketAwaiter &&) noexcept = default;
  bool await_ready() const noexcept {
    return consumed_ || operation_.await_ready();
  }
  bool await_suspend(std::coroutine_handle<> continuation) noexcept {
    // 通用引擎负责 opcode 选择、SQE 提交、取消排空与 CQE 结果回填。
    return operation_.await_suspend(continuation);
  }
  auto await_resume() -> expected<Socket> {
    // 同一个创建结果只允许接管一次，避免重复建立 fd 关闭责任。
    if (std::exchange(consumed_, true))
      return std::unexpected{make_error(EINVAL)};
    auto descriptor = operation_.await_resume();
    if (!descriptor)
      return std::unexpected{descriptor.error()};
    // 只有成功完成才建立唯一 fd 所有者；取消获胜的成功 fd 由引擎处理。
    owned_native_socket guard{*descriptor};
    // 平台导入约束与 macOS SIGPIPE 配置保持一致，不执行额外网络 IO。
    if (auto result = Socket::prepare(guard.get()); !result)
      return std::unexpected{result.error()};
    auto socket = Socket::adopt_checked(guard.get(), std::move(context_));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)guard.release(); // 注册成功才将关闭责任转给稳定资源控制块。
    return socket;
  }

private:
  io::io_context context_; // 固定创建与导入的同一归属，恢复线程不改变它。
  decltype(io::socket(0, 0,
                      0)) operation_; // 唯一原生请求，不建立第二套状态机。
  bool consumed_{};
};
inline auto Socket::create_async(int domain, int type, int protocol,
                                 io::io_context context)
    -> CreateSocketAwaiter {
  return {domain, type, protocol, std::move(context)};
}
} // namespace faio::net::detail
#endif
