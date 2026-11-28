/**
 * @file faio/detail/net/base/socket.hpp
 * @brief 套接字资源封装。
 */
#ifndef FAIO_DETAIL_NET_BASE_SOCKET_HPP
#define FAIO_DETAIL_NET_BASE_SOCKET_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/net/traits/socket_address.hpp"
#include "faio/detail/net/platform/socket_association.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/core/context.hpp"
#include "faio/detail/io/core/file_descriptor.hpp"
#include "faio/detail/io/awaiter/shutdown.hpp"
#include "faio/detail/io/awaiter/socket.hpp"
#include <fcntl.h>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace faio::net::detail {
class create_socket_awaiter;
class socket;

/**  @brief 独占原生 socket 的 RAII 所有者，平台关联策略不改变统一的所有权约定。 */
class owned_native_socket {
 public:
  /** @brief 构造不持有原生 socket 的空拥有者。 */
  owned_native_socket() noexcept = default;

  /** @brief 接管原生 socket 的关闭责任。 */
  explicit owned_native_socket(native_socket_type fd) noexcept : fd_(fd) {}

  /** @brief 关闭尚未转出的原生 socket。 */
  ~owned_native_socket() { reset(); }

  /** @brief 禁止复制此对象。 */
  owned_native_socket(const owned_native_socket&) = delete;
  /** @brief 禁止复制赋值。 */
  owned_native_socket& operator=(const owned_native_socket&) = delete;

  /** @brief 接管源对象的状态和资源租约。 */
  owned_native_socket(owned_native_socket&& other) noexcept
      : fd_(std::exchange(other.fd_, native_socket_type{-1})),
        association_(std::move(other.association_)) {}

  /** @brief 移动赋值资源及状态。 */
  owned_native_socket& operator=(owned_native_socket&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, native_socket_type{-1});
      association_ = std::move(other.association_);
    }
    return *this;
  }

  /** @brief 借用原生 socket 句柄，不转移关闭责任。 */
  [[nodiscard]] auto get() const noexcept -> native_socket_type { return fd_; }

  /** @brief 按当前类型约定移交或归还资源责任，并将源对象置空。 */
  [[nodiscard]] auto release() noexcept -> native_socket_type {
    association_.forget(fd_);
    return std::exchange(fd_, native_socket_type{-1});
  }

  /** @brief 判断拥有者是否持有有效原生 socket。 */
  explicit operator bool() const noexcept { return fd_ >= 0; }

  /** @brief 关闭尚未转出的原生 socket 并清空拥有者。 */
  void reset() noexcept {
    association_.forget(fd_);
    if (fd_ >= 0)
      close_native_socket(std::exchange(fd_, native_socket_type{-1}));
  }

  /** @brief 验证原生拥有者能否导入目标 IO 上下文。 */
  [[nodiscard]] auto validate_import(const io::io_context& context) const noexcept
      -> expected<void> {
    return association_.validate(context);
  }

 private:
  friend class socket;

  /** @brief 接管原生 socket 的关闭责任。 */
  owned_native_socket(native_socket_type fd, native_socket_association association) noexcept
      : fd_(fd), association_(std::move(association)) {}

  /** @brief 成功导入后转移句柄并清空原生拥有者。 */
  auto release_after_import() noexcept -> native_socket_type {
    association_.release_imported();
    return std::exchange(fd_, native_socket_type{-1});
  }

  native_socket_type fd_{-1}; /**< 底层 socket 的原生句柄，完整保留平台句柄宽度。 */
  [[no_unique_address]] native_socket_association
      association_; /**< 平台专用的 socket 关联状态，Windows 用于固定 IOCP 归属。 */
};

/**  @brief 与后端无关的 socket；控制块绑定创建时的 I/O 域，移动不改变归属。 */
class socket : public io::detail::file_descriptor {
 public:
  /** @brief 接管 socket 资源并保存其 IO 上下文归属。 */
  explicit socket(native_socket_type fd, io::io_context context = io::io_context::current())
      : file_descriptor{fd, std::move(context)} {}

  /** @brief 接管 socket 资源并保存其 IO 上下文归属。 */
  explicit socket(std::shared_ptr<io::detail::resource_state> resource)
      : file_descriptor{std::move(resource)} {}

  /** @brief 接管源对象的状态和资源租约。 */
  socket(socket&&) noexcept = default;

  /** @brief 移动赋值资源及状态。 */
  auto operator=(socket&&) noexcept -> socket& = default;

  /** @brief 禁止复制此对象。 */
  socket(const socket&) = delete;

  /** @brief 禁止复制赋值。 */
  auto operator=(const socket&) -> socket& = delete;

  /** @brief 释放 socket 的共享资源租约。 */
  ~socket() = default;

  /**  @brief 独立方向半部共享同一控制块，绝不复制原生 handle。 */
  [[nodiscard]] auto share() const -> socket { return socket{resource()}; }

  /** @brief 将 socket 绑定到给定本地地址。 */
  template <class Addr>
    requires socket_address<Addr>
  [[nodiscard]] auto bind(const Addr& address) -> expected<void> {
    return io::detail::with_resource(resource(), io::interest::NONE, [&]() -> expected<void> {
      if (::bind(fd(), address.sockaddr(), address.length()) < 0)
        return std::unexpected{socket_error()};
      return {};
    });
  }

  /** @brief 进入监听状态并设置待接纳连接队列上限。 */
  [[nodiscard]] auto listen(int backlog = SOMAXCONN) -> expected<void> {
    if (backlog < 0)
      return std::unexpected{make_error(EINVAL)};
    return io::detail::with_resource(resource(), io::interest::NONE, [&]() -> expected<void> {
      if (::listen(fd(), backlog) < 0)
        return std::unexpected{socket_error()};
      return {};
    });
  }

  /** @brief 请求关闭 socket 的指定传输方向。 */
  auto shutdown(io::shutdown_behavior how) noexcept {
    return io::shutdown(resource(), static_cast<int>(how));
  }

  /** @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。 */
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    auto result = native_socket_association::detach(resource(),
                                                    [&] { return file_descriptor::into_native(); });
    if (!result)
      return std::unexpected{result.error()};
    return owned_native_socket{*result, native_socket_association::capture(resource())};
  }

  /**  @brief 仅完成 checked import 后调用，避免销毁旧拥有者时删掉新资源的注册。 */
  static void release_imported(owned_native_socket& native) noexcept {
    (void)native.release_after_import();
  }

  /**  @brief 统一设置 nonblocking/CLOEXEC；macOS 不假设支持 socket type 标志。 */
  [[nodiscard]] static auto prepare(native_socket_type descriptor) noexcept -> expected<void> {
    return prepare_native_socket(descriptor);
  }

  /**
   *  @brief 失败不接管原生 fd；将 domain 停止、容量耗尽及注册失败转成
   * expected。
   * @details 调用方保留 native guard，直到本方法成功再
   * release，异常期间不丢失关闭责任。
   */
  [[nodiscard]] static auto adopt_checked(native_socket_type descriptor, io::io_context context)
      -> expected<socket> {
    try {
      return socket{descriptor, std::move(context)};
    } catch (const std::system_error& error) {
      return std::unexpected{socket_error_from_code(error.code().value())};
    } catch (const std::bad_alloc&) {
      return std::unexpected{make_error(ENOMEM)};
    } catch (const std::overflow_error&) {
      return std::unexpected{make_error(EOVERFLOW)};
    }
  }

  /**  @brief 导入原生句柄时校验地址族，阻止把 Unix handle 当作 IP handle 解释。 */
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
      return std::unexpected{make_error(::faio::error::INVALID_SOCKET_TYPE)};
    return {};
  }

  /** @brief 创建 socket 并绑定到指定 IO 上下文。 */
  template <class T = socket>
  [[nodiscard]] static auto create(int domain,
                                   int type,
                                   int protocol,
                                   io::io_context context = io::io_context::current())
      -> expected<T> {
    auto descriptor = create_native_socket(domain, type, protocol);
    if (!descriptor)
      return std::unexpected{descriptor.error()};
    owned_native_socket guard{*descriptor};
    if (auto prepared = prepare_created_native_socket(*descriptor); !prepared)
      return std::unexpected{prepared.error()};
    auto created_socket = adopt_checked(*descriptor, std::move(context));
    if (!created_socket)
      return std::unexpected{created_socket.error()};
    (void)guard.release();
    if constexpr (std::same_as<T, socket>)
      return created_socket;
    else
      return T{std::move(*created_socket)};
  }

  /**
   *  @brief 供异步连接组合直接创建 socket，已有原生 opcode 使用 SOCKET
   * SQE/CQE。
   * @param context 长期 IO 归属；空 context 在协程首次运行时确定为当前 domain。
   * @details 复用 raw socket awaiter，不先执行 ::socket，不增加 readiness
   * 观察， 也不将原生请求交给应用线程池。同步 create 的 expected 约定保持独立。
   */
  [[nodiscard]] static auto create_async(int domain, int type, int protocol, io::io_context context)
      -> create_socket_awaiter;
};

/**
 *  @brief 转换原生 socket 创建结果的所有权，不分配额外 task 协程帧。
 * @details 挂起与恢复直接委托现有 raw awaiter；本类型仅在成功完成后导入 fd。
 */
class create_socket_awaiter {
 public:
  /** @brief 构造异步 socket 创建等待器。 */
  create_socket_awaiter(int domain, int type, int protocol, io::io_context context)
      : context_{context ? std::move(context) : io::io_context::current()},
        operation_{io::socket(domain, type, protocol).with_context(context_)} {}

  /** @brief 禁止复制此对象。 */
  create_socket_awaiter(const create_socket_awaiter&) = delete;

  /** @brief 接管源对象的状态和资源租约。 */
  create_socket_awaiter(create_socket_awaiter&&) noexcept = default;

  /** @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。 */
  bool await_ready() const noexcept { return consumed_ || operation_.await_ready(); }

  /** @brief 登记恢复目标，按等待协议交接挂起与完成责任。 */
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    return operation_.await_suspend(continuation);
  }

  /** @brief 完成交接后领取结果，并按接口约定传播失败或取消。 */
  auto await_resume() -> expected<socket> {
    if (std::exchange(consumed_, true))
      return std::unexpected{make_error(EINVAL)};
    auto descriptor = operation_.await_resume();
    if (!descriptor)
      return std::unexpected{descriptor.error()};
    owned_native_socket guard{*descriptor};
    if (auto result = prepare_created_native_socket(guard.get()); !result)
      return std::unexpected{result.error()};
    auto socket = socket::adopt_checked(guard.get(), std::move(context_));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)guard.release();
    return socket;
  }

 private:
  io::io_context context_;                  /**< 固定创建与导入的同一归属，恢复线程不改变它。 */
  decltype(io::socket(0, 0, 0)) operation_; /**< 唯一原生请求，不建立第二套状态机。 */
  bool consumed_{};                         /**< 结果或输入是否已被消费，用于防止重复领取。 */
};

/** @brief 异步创建 socket 并交付指定传输类型。 */
inline auto socket::create_async(int domain, int type, int protocol, io::io_context context)
    -> create_socket_awaiter {
  return {domain, type, protocol, std::move(context)};
}
}  // namespace faio::net::detail
#endif
