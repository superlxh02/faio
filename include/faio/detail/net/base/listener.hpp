/**
 * @file faio/detail/net/base/listener.hpp
 * @brief 监听器与批量接受。
 */
#ifndef FAIO_DETAIL_NET_BASE_LISTENER_HPP
#define FAIO_DETAIL_NET_BASE_LISTENER_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/io/awaiter/accept.hpp"
#include "faio/detail/io/awaiter/ready.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/net/accept_options.hpp"
#include "faio/detail/net/base/address.hpp"
#include "faio/detail/net/base/awaiter_options.hpp"
#include "faio/detail/net/base/socket.hpp"
#include <span>
#include <vector>

namespace faio::net::detail {
/**
 *  @brief 通用 TCP/Unix listener，新连接在发布前按 accept_options 固定 IO
 * 归属。
 */
template <class Listener, class Stream, class Addr>
class base_listener : public impl_local_addr<base_listener<Listener, Stream, Addr>, Addr> {
 protected:
  /** @brief 接管源对象的状态和资源租约。 */
  explicit base_listener(socket&& socket) : socket_{std::move(socket)} {}

 public:
  /** @brief 接管源对象的状态和资源租约。 */
  base_listener(base_listener&&) noexcept = default;

  /** @brief 移动赋值资源及状态。 */
  auto operator=(base_listener&&) noexcept -> base_listener& = default;

  /** @brief 禁止复制此对象。 */
  base_listener(const base_listener&) = delete;

  /** @brief 禁止复制赋值。 */
  auto operator=(const base_listener&) -> base_listener& = delete;

  /** @brief 释放监听 socket 的资源租约。 */
  ~base_listener() = default;

  /**  @brief 一次 accept 不创建 task 帧，地址及长度由最终 awaiter 自身持有。 */
  class accept_awaiter : public awaiter_options<accept_awaiter> {
   public:
    /** @brief 构造保存对端地址及接纳请求的等待器。 */
    accept_awaiter(std::shared_ptr<io::detail::resource_state> resource,
                   io::io_context context,
                   accept_options options = {},
                   bool native_no_wait = false)
        : resource_{std::move(resource)},
          context_{std::move(context)},
          options_{std::move(options)},
          native_no_wait_{native_no_wait},
          operation_{io::accept(resource_, address_.sockaddr(), &length_, 0)} {
      if (native_no_wait_)
        operation_.no_wait();
    }

    /** @brief 禁止复制此对象。 */
    accept_awaiter(const accept_awaiter&) = delete;

    /**
     *  @brief 提交前移动会重建 sockaddr/长度指针，保障 task await_transform
     * 的移动安全。
     */
    accept_awaiter(accept_awaiter&& other) noexcept
        : awaiter_options<accept_awaiter>{std::move(other)},
          resource_{std::move(other.resource_)},
          context_{std::move(other.context_)},
          options_{std::move(other.options_)},
          native_no_wait_{other.native_no_wait_},
          operation_{io::accept(resource_, address_.sockaddr(), &length_, 0)},
          owner_{other.owner_},
          consumed_{other.consumed_} {
      if (native_no_wait_)
        operation_.no_wait();
    }

    /** @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。 */
    auto reservation(void* owner) & noexcept -> accept_awaiter& {
      owner_ = owner;
      return *this;
    }

    /** @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。 */
    auto reservation(void* owner) && noexcept -> accept_awaiter&& {
      owner_ = owner;
      return std::move(*this);
    }

    /** @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。 */
    auto await_ready() noexcept { return consumed_ || operation_.await_ready(); }

    /** @brief 登记恢复目标，按等待协议交接挂起与完成责任。 */
    template <class Promise>
    auto await_suspend(std::coroutine_handle<Promise> continuation) {
      this->configure(operation_);
      operation_.reservation(owner_);
      return operation_.await_suspend(continuation);
    }

    /** @brief 完成交接后领取结果，并按接口约定传播失败或取消。 */
    auto await_resume() -> expected<std::pair<Stream, Addr>> {
      if (std::exchange(consumed_, true))
        return std::unexpected{make_error(EINVAL)};
      auto result = operation_.await_resume();
      if (!result)
        return std::unexpected{result.error()};
      if constexpr (requires { address_.set_length(length_); })
        address_.set_length(length_);
      return adopt_accepted(owned_native_socket{*result}, address_, resource_, context_, options_);
    }

   private:
    std::shared_ptr<io::detail::resource_state>
        resource_;   /**< 共享资源控制块的租约，在挂起期间保持句柄身份及所属 I/O 域不变。 */
    Addr address_{}; /**< 拥有的本地或对端 socket 地址，提交期间不借用临时对象。 */
    socklen_t length_{Addr::capacity()}; /**< 原生 socket 地址的实际长度，含平台要求的地址头部。 */
    io::io_context context_; /**< 持有所属 IO 上下文租约，用于资源归属、服务选择与安全排空。 */
    accept_options options_; /**< 接受连接时使用的 socket 配置副本。 */
    bool native_no_wait_{};  /**< 接受连接是否选择原生非等待扩展路径。 */
    decltype(io::accept(
        std::declval<std::shared_ptr<io::detail::resource_state>>(),
        nullptr,
        nullptr,
        0)) operation_; /**< 地址固定的底层异步操作，由包装器负责启动、等待和交付结果。 */
    void* owner_{};     /**< 组合操作的稳定身份，用于验证读写方向的排他预留。 */
    bool consumed_{};   /**< 结果或输入是否已被消费，用于防止重复领取。 */
  };

  /**  @brief 接受一个连接，默认在同一 runtime 的活跃 IO domain 中均衡首次归属。 */
  auto accept(accept_options options = {}) const noexcept {
    return accept_awaiter{resource(), context(), std::move(options)};
  }

  /**  @brief 显式指定新连接归属；已发布 fd 不发生 domain 迁移。 */
  auto accept(io::io_context target) const noexcept {
    return accept(accept_options{accept_placement::EXPLICIT_CONTEXT, std::move(target)});
  }

  /**  @brief 同步接受一个已经完成握手的连接；EAGAIN 不挂起。 */
  [[nodiscard]] auto try_accept(accept_options options = {}) const
      -> expected<std::pair<Stream, Addr>> {
    return try_accept_impl(resource(), context(), options);
  }

  /** @brief 即时尝试接纳一个连接，不等待监听 socket 就绪。 */
  [[nodiscard]] auto try_accept(io::io_context target) const -> expected<std::pair<Stream, Addr>> {
    return try_accept(accept_options{accept_placement::EXPLICIT_CONTEXT, std::move(target)});
  }

  /**
   *  @brief 有界接受批次，等待首条连接，其余只领取当前已排队的连接。
   * @param maximum 批次上限，零返回 EINVAL。
   * @param options 每个新连接的首次 IO 归属策略。
   * @details io_uring 后续项提交 ACCEPT_DONTWAIT 并等待真实 CQE；能力不支持时
   *          返回首条，不等待填满批次。epoll/kqueue 使用非阻塞 accept
   * 排空已有连接。
   */
  auto accept_many(std::size_t maximum = 64, accept_options options = {}) const
      -> task<expected<std::vector<std::pair<Stream, Addr>>>> {
    return accept_many_impl(resource(), context(), maximum, std::move(options));
  }

  /** @brief 等待首个连接后，继续接纳已就绪连接至数量上限。 */
  auto accept_many(std::size_t maximum, io::io_context target) const
      -> task<expected<std::vector<std::pair<Stream, Addr>>>> {
    return accept_many(maximum,
                       accept_options{accept_placement::EXPLICIT_CONTEXT, std::move(target)});
  }

  /** @brief 异步关闭共享套接字，等待在途操作安全结束。 */
  auto close() noexcept { return socket_.close(); }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto fd() const noexcept -> native_socket_type { return socket_.fd(); }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] auto as_native_handle() const noexcept -> native_socket_type { return fd(); }

  /** @brief 取得底层共享资源控制块，保持句柄身份及所属域有效。 */
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }

  /** @brief 取得该对象关联的 I/O 上下文租约。 */
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }

  /** @brief 等待给定的 socket 就绪条件。 */
  auto ready(io::interest interest = io::interest::READABLE) const noexcept {
    return io::ready(resource(), interest);
  }

  /** @brief 等待 socket 或管道可读，不消费数据。 */
  auto readable() const noexcept { return ready(); }

  /** @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。 */
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }

  /** @brief 接管显式传入的原生句柄，按平台规则建立资源包装。 */
  [[nodiscard]] static auto from_native(io::io_context context, owned_native_socket native)
      -> expected<Listener> {
    if (auto placement = native.validate_import(context); !placement)
      return std::unexpected{placement.error()};
    int accepting{};
    socklen_t length = sizeof(accepting);
    if (socket_getsockopt(native.get(), SOL_SOCKET, SO_ACCEPTCONN, &accepting, &length) < 0)
      return std::unexpected{socket_error()};
    if (!accepting)
      return std::unexpected{make_error(::faio::error::INVALID_SOCKET_TYPE)};
    if (auto valid = socket::validate_family<Addr>(native.get()); !valid)
      return std::unexpected{valid.error()};
    if (auto result = socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    socket::release_imported(native);
    return Listener{std::move(*socket)};
  }

  /** @brief 将 socket 绑定到给定本地地址。 */
  [[nodiscard]] static auto bind(const Addr& address,
                                 io::io_context context = io::io_context::current(),
                                 int backlog = SOMAXCONN) -> expected<Listener> {
    auto socket = socket::create(address.family(), SOCK_STREAM, 0, std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    if (auto result = socket->bind(address); !result)
      return std::unexpected{result.error()};
    if (auto result = socket->listen(backlog); !result)
      return std::unexpected{result.error()};
    return Listener{std::move(*socket)};
  }

  /** @brief 将 socket 绑定到给定本地地址。 */
  [[nodiscard]] static auto bind(io::io_context context,
                                 const Addr& address,
                                 int backlog = SOMAXCONN) -> expected<Listener> {
    return bind(address, std::move(context), backlog);
  }

  /** @brief 将 socket 绑定到给定本地地址。 */
  [[nodiscard]] static auto bind(std::span<const Addr> addresses,
                                 io::io_context context = io::io_context::current())
      -> expected<Listener> {
    error last{::faio::error::INVALID_ADDRESSES};
    for (const auto& address : addresses) {
      auto result = bind(address, context);
      if (result)
        return result;
      last = result.error();
    }
    return std::unexpected{last};
  }

  /** @brief 将 socket 绑定到给定本地地址。 */
  [[nodiscard]] static auto bind(io::io_context context, std::span<const Addr> addresses)
      -> expected<Listener> {
    return bind(addresses, std::move(context));
  }

 private:
  /**
   *  @brief 根据 listener 最终归属选定目标，不把停机后的空分配结果当作 lazy
   * 资源。
   */
  static auto select_context(const std::shared_ptr<io::detail::resource_state>& resource,
                             const io::io_context& context,
                             const accept_options& options) -> expected<io::io_context> {
    const auto source = resource && resource->owner ? io::io_context{resource->owner} : context;
    switch (options.placement) {
      case accept_placement::LISTENER_LOCAL:
        return source;
      case accept_placement::BALANCED: {
        if (!source)
          return source;
        auto target = source.balanced_context();
        if (!target)
          return std::unexpected{make_error(ECANCELED)};
        return target;
      }
      case accept_placement::EXPLICIT_CONTEXT:
        if (!options.target)
          return std::unexpected{make_error(EINVAL)};
        return options.target;
    }
    return std::unexpected{make_error(EINVAL)};
  }

  /**  @brief 唯一 guard 保留关闭责任，只有 target 注册成功才 release 到 stream。 */
  static auto adopt_accepted(owned_native_socket native,
                             Addr address,
                             const std::shared_ptr<io::detail::resource_state>& resource,
                             const io::io_context& context,
                             const accept_options& options) -> expected<std::pair<Stream, Addr>> {
    if (auto prepared = prepare_created_native_socket(native.get()); !prepared)
      return std::unexpected{prepared.error()};
    auto target = select_context(resource, context, options);
    if (!target)
      return std::unexpected{target.error()};
    auto socket = socket::adopt_checked(native.get(), std::move(*target));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return std::pair{Stream{std::move(*socket)}, std::move(address)};
  }

  /** @brief 即时接纳连接并构建带对端地址的流结果。 */
  static auto try_accept_impl(const std::shared_ptr<io::detail::resource_state>& resource,
                              const io::io_context& context,
                              const accept_options& options,
                              void* owner = nullptr) -> expected<std::pair<Stream, Addr>> {
    auto accepted = io::detail::with_resource(
        resource,
        io::interest::READABLE,
        [&]() -> expected<std::pair<owned_native_socket, Addr>> {
          Addr address{};
          socklen_t length = Addr::capacity();
          const auto descriptor = accept_native_socket(resource->fd(), address.sockaddr(), &length);
          if (descriptor < 0)
            return std::unexpected{socket_error()};
          if constexpr (requires { address.set_length(length); })
            address.set_length(length);
          return std::pair{owned_native_socket{descriptor}, std::move(address)};
        },
        owner);
    if (!accepted)
      return std::unexpected{accepted.error()};
    return adopt_accepted(
        std::move(accepted->first), std::move(accepted->second), resource, context, options);
  }

  /**  @brief 批量 accept 保存调用时资源，并在整个批次保持读方向租约。 */
  static auto accept_many_impl(std::shared_ptr<io::detail::resource_state> resource,
                               io::io_context context,
                               std::size_t maximum,
                               accept_options options)
      -> task<expected<std::vector<std::pair<Stream, Addr>>>> {
    if (!maximum)
      co_return std::unexpected{make_error(EINVAL)};
    char owner_identity{};
    auto reservation =
        io::detail::reserve_direction(resource, io::interest::READABLE, &owner_identity);
    if (!reservation)
      co_return std::unexpected{reservation.error()};
    std::vector<std::pair<Stream, Addr>> accepted;
    accepted.reserve(maximum);
    auto first = co_await accept_awaiter{resource, context, options}.reservation(&owner_identity);
    if (!first)
      co_return std::unexpected{first.error()};
    accepted.push_back(std::move(*first));
    const bool native = resource->owner->supports_native(io::detail::operation_kind::ACCEPT);
    const bool native_no_wait = resource->owner->capabilities().native_accept_nowait;
    while (accepted.size() < maximum) {
      if (native && !native_no_wait)
        break;
      auto next = native ? co_await accept_awaiter{resource, context, options, true}.reservation(
                               &owner_identity)
                         : try_accept_impl(resource, context, options, &owner_identity);
      if (!next) {
        if (socket_would_block(next.error()))
          break;
        co_return std::unexpected{next.error()};
      }
      accepted.push_back(std::move(*next));
    }
    co_return accepted;
  }

  socket socket_; /**< 持有或共享底层 socket 包装，在挂起期间保持原生资源身份有效。 */
};
}  // namespace faio::net::detail
#endif
