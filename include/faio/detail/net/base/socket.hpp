/**
 * @file faio/detail/net/base/socket.hpp
 * @brief 网络地址、socket 与传输协议：socket 资源封装。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#ifndef FAIO_DETAIL_NET_BASE_SOCKET_HPP
#define FAIO_DETAIL_NET_BASE_SOCKET_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/net/traits/socket_address.hpp"
#include "faio/detail/net/platform/socket_association.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/core/context.hpp"
#include "faio/detail/io/io.hpp"
#include <fcntl.h>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace faio::net::detail {
class CreateSocketAwaiter;
class Socket;

/** @brief 独占原生 socket 的 RAII 所有者，平台关联策略不改变统一拥有协议。 */
class owned_native_socket {
 public:
  owned_native_socket() noexcept = default;

  explicit owned_native_socket(native_socket_type fd) noexcept : fd_(fd) {}

  ~owned_native_socket() { reset(); }

  owned_native_socket(const owned_native_socket&) = delete;
  owned_native_socket& operator=(const owned_native_socket&) = delete;

  owned_native_socket(owned_native_socket&& other) noexcept
      : fd_(std::exchange(other.fd_, native_socket_type{-1})),
        association_(std::move(other.association_)) {}

  owned_native_socket& operator=(owned_native_socket&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, native_socket_type{-1});
      association_ = std::move(other.association_);
    }
    return *this;
  }

  /// @brief 取得当前对象保存的值或借用句柄，是否转交所有权由返回类型决定。
  [[nodiscard]] auto get() const noexcept -> native_socket_type { return fd_; }

  /// @brief 转出或归还当前类型约定的资源责任，并把源对象变为空状态。
  [[nodiscard]] auto release() noexcept -> native_socket_type {
    association_.forget(fd_);
    return std::exchange(fd_, native_socket_type{-1});
  }

  explicit operator bool() const noexcept { return fd_ >= 0; }

  /// @brief 按该类型约定释放或清空当前状态，重复调用遵循对象的空状态规则。
  void reset() noexcept {
    association_.forget(fd_);
    if (fd_ >= 0)
      close_native_socket(std::exchange(fd_, native_socket_type{-1}));
  }

  [[nodiscard]] auto validate_import(const io::io_context& context) const noexcept
      -> expected<void> {
    return association_.validate(context);
  }

 private:
  friend class Socket;

  owned_native_socket(native_socket_type fd, native_socket_association association) noexcept
      : fd_(fd), association_(std::move(association)) {}

  auto release_after_import() noexcept -> native_socket_type {
    association_.release_imported();
    return std::exchange(fd_, native_socket_type{-1});
  }

  native_socket_type fd_{-1};  ///< 底层 socket 的原生句柄，完整保留平台句柄宽度。
  [[no_unique_address]] native_socket_association association_;  ///< 平台专用的 socket 关联状态，Windows 用于固定 IOCP 归属。
};

/** @brief 后端中立 socket；控制块绑定创建时的 IO domain，移动不改变归属。 */
class Socket : public io::detail::FileDescriptor {
 public:
  explicit Socket(native_socket_type fd, io::io_context context = io::io_context::current())
      : FileDescriptor{fd, std::move(context)} {}

  explicit Socket(std::shared_ptr<io::detail::resource_state> resource)
      : FileDescriptor{std::move(resource)} {}

  Socket(Socket&&) noexcept = default;

  auto operator=(Socket&&) noexcept -> Socket& = default;

  Socket(const Socket&) = delete;

  auto operator=(const Socket&) -> Socket& = delete;

  ~Socket() = default;

  /** @brief 独立方向半边共享同一控制块，绝不复制原生 handle。 */
  [[nodiscard]] auto share() const -> Socket { return Socket{resource()}; }

  template <class Addr>
    requires socket_address<Addr>
  [[nodiscard]] auto bind(const Addr& address) -> expected<void> {
    return io::detail::with_resource(resource(), io::Interest::none, [&]() -> expected<void> {
      if (::bind(fd(), address.sockaddr(), address.length()) < 0)
        return std::unexpected{socket_error()};
      return {};
    });
  }

  [[nodiscard]] auto listen(int backlog = SOMAXCONN) -> expected<void> {
    if (backlog < 0)
      return std::unexpected{make_error(EINVAL)};
    return io::detail::with_resource(resource(), io::Interest::none, [&]() -> expected<void> {
      if (::listen(fd(), backlog) < 0)
        return std::unexpected{socket_error()};
      return {};
    });
  }

  /// @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。
  auto shutdown(io::ShutdownBehavior how) noexcept {
    return io::shutdown(resource(), static_cast<int>(how));
  }

  /// @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    auto result = native_socket_association::detach(resource(),
                                                    [&] { return FileDescriptor::into_native(); });
    if (!result)
      return std::unexpected{result.error()};
    return owned_native_socket{*result, native_socket_association::capture(resource())};
  }

  /** @brief 仅完成 checked import 后调用，避免销毁旧拥有者时删掉新资源的注册。 */
  static void release_imported(owned_native_socket& native) noexcept {
    (void)native.release_after_import();
  }

  /** @brief 统一设置 nonblocking/CLOEXEC；macOS 不假设支持 socket type 标志。
   */
  [[nodiscard]] static auto prepare(native_socket_type descriptor) noexcept -> expected<void> {
    return prepare_native_socket(descriptor);
  }

  /** @brief 失败不接管原生 fd；将 domain 停止、容量耗尽及注册失败转成
   * expected。
   * @details 调用方保留 native guard，直到本方法成功再
   * release，异常期间不丢失关闭责任。
   */
  [[nodiscard]] static auto adopt_checked(native_socket_type descriptor, io::io_context context)
      -> expected<Socket> {
    try {
      return Socket{descriptor, std::move(context)};
    } catch (const std::system_error& error) {
      return std::unexpected{socket_error_from_code(error.code().value())};
    } catch (const std::bad_alloc&) {
      return std::unexpected{make_error(ENOMEM)};
    } catch (const std::overflow_error&) {
      return std::unexpected{make_error(EOVERFLOW)};
    }
  }

  /** @brief 原生导入时校验家族，阻止把 Unix handle 当作 IP handle 解释。 */
  template <class Addr>
  [[nodiscard]] static auto validate_family(native_socket_type descriptor) noexcept
      -> expected<void> {
    const auto family = socket_address_family(descriptor);
    if (!family)
      return std::unexpected{family.error()};
    const int expected_family = Addr{}.family();
    const bool valid =
        expected_family == AF_UNIX ? *family == AF_UNIX : *family == AF_INET || *family == AF_INET6;
    if (!valid)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    return {};
  }

  template <class T = Socket>
  [[nodiscard]] static auto create(int domain,
                                   int type,
                                   int protocol,
                                   io::io_context context = io::io_context::current())
      -> expected<T> {
    auto descriptor = create_native_socket(domain, type, protocol);
    if (!descriptor)
      return std::unexpected{descriptor.error()};
    owned_native_socket guard{*descriptor};
    if (auto prepared = prepare(*descriptor); !prepared)
      return std::unexpected{prepared.error()};
    auto socket = adopt_checked(*descriptor, std::move(context));
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
  [[nodiscard]] static auto create_async(int domain, int type, int protocol, io::io_context context)
      -> CreateSocketAwaiter;
};

/** @brief 原生 socket 创建结果的拥有权转换，不分配额外 task 协程帧。
 * @details 挂起与恢复直接委托现有 raw awaiter；本类型仅在成功完成后导入 fd。
 */
class CreateSocketAwaiter {
 public:
  CreateSocketAwaiter(int domain, int type, int protocol, io::io_context context)
      : context_{context ? std::move(context) : io::io_context::current()},
        operation_{io::socket(domain, type, protocol).with_context(context_)} {}

  CreateSocketAwaiter(const CreateSocketAwaiter&) = delete;

  CreateSocketAwaiter(CreateSocketAwaiter&&) noexcept = default;

  /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
  bool await_ready() const noexcept { return consumed_ || operation_.await_ready(); }

  /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
  bool await_suspend(std::coroutine_handle<> continuation) noexcept {
    /// @brief 通用引擎负责 opcode 选择、SQE 提交、取消排空与 CQE 结果回填。
    return operation_.await_suspend(continuation);
  }

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  auto await_resume() -> expected<Socket> {
    /// @brief 同一个创建结果只允许接管一次，避免重复建立 fd 关闭责任。
    if (std::exchange(consumed_, true))
      return std::unexpected{make_error(EINVAL)};
    auto descriptor = operation_.await_resume();
    if (!descriptor)
      return std::unexpected{descriptor.error()};
    /// @brief 只有成功完成才建立唯一 fd 所有者；取消获胜的成功 fd 由引擎处理。
    owned_native_socket guard{*descriptor};
    /// @brief 平台导入约束与 macOS SIGPIPE 配置保持一致，不执行额外网络 IO。
    if (auto result = Socket::prepare(guard.get()); !result)
      return std::unexpected{result.error()};
    auto socket = Socket::adopt_checked(guard.get(), std::move(context_));
    if (!socket)
      return std::unexpected{socket.error()};
    /// @brief 注册成功才将关闭责任转给稳定资源控制块。
    (void)guard.release();
    return socket;
  }

 private:
  io::io_context context_;  ///< 固定创建与导入的同一归属，恢复线程不改变它。
  decltype(io::socket(0, 0,
                      0)) operation_;  ///< 唯一原生请求，不建立第二套状态机。
  bool consumed_{};  ///< 结果或输入是否已消费，防止重复领取。
};

inline auto Socket::create_async(int domain, int type, int protocol, io::io_context context)
    -> CreateSocketAwaiter {
  return {domain, type, protocol, std::move(context)};
}
}
#endif
