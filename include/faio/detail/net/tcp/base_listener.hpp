#ifndef FAIO_DETAIL_NET_TCP_BASE_LISTENER_HPP
#define FAIO_DETAIL_NET_TCP_BASE_LISTENER_HPP
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/net/common/accept_options.hpp"
#include "faio/detail/net/common/addr_util.hpp"
#include "faio/detail/net/common/awaiter_options.hpp"
#include "faio/detail/net/common/socket.hpp"
#include <span>
#include <vector>
namespace faio::net::detail {
/** @brief 通用 TCP/Unix listener，新连接在发布前按 accept_options 固定 IO
 * 归属。
 */
template <class Listener, class Stream, class Addr>
class BaseListener
    : public ImplLocalAddr<BaseListener<Listener, Stream, Addr>, Addr> {
protected:
  explicit BaseListener(Socket &&socket) : socket_{std::move(socket)} {}

public:
  BaseListener(BaseListener &&) noexcept = default;
  auto operator=(BaseListener &&) noexcept -> BaseListener & = default;
  BaseListener(const BaseListener &) = delete;
  auto operator=(const BaseListener &) -> BaseListener & = delete;
  ~BaseListener() = default;
  /** @brief 一次 accept 不创建 task 帧，地址及长度由最终 awaiter 自身持有。 */
  class Accept : public AwaiterOptions<Accept> {
  public:
    Accept(std::shared_ptr<io::detail::resource_state> resource,
           io::io_context context, accept_options options = {},
           bool native_no_wait = false)
        : resource_{std::move(resource)}, context_{std::move(context)},
          options_{std::move(options)}, native_no_wait_{native_no_wait},
          operation_{io::accept(resource_, address_.sockaddr(), &length_, 0)} {
      if (native_no_wait_)
        operation_.no_wait(); // 只准备原生即时 ACCEPT；EAGAIN 由 CQE 返回。
    }
    Accept(const Accept &) = delete;
    /** @brief 提交前移动会重建 sockaddr/长度指针，保障 task await_transform
     * 的移动安全。 */
    Accept(Accept &&other) noexcept
        : AwaiterOptions<Accept>{std::move(other)},
          resource_{std::move(other.resource_)},
          context_{std::move(other.context_)},
          options_{std::move(other.options_)},
          native_no_wait_{other.native_no_wait_},
          operation_{io::accept(resource_, address_.sockaddr(), &length_, 0)},
          owner_{other.owner_}, consumed_{other.consumed_} {
      if (native_no_wait_)
        operation_.no_wait(); // 移动时连同即时语义重建指向新地址存储的请求。
    }
    auto reservation(void *owner) & noexcept -> Accept & {
      owner_ = owner;
      return *this;
    }
    auto reservation(void *owner) && noexcept -> Accept && {
      owner_ = owner;
      return std::move(*this);
    }
    auto await_ready() noexcept {
      return consumed_ || operation_.await_ready();
    }
    auto await_suspend(std::coroutine_handle<> continuation) {
      this->configure(operation_);
      operation_.reservation(owner_);
      return operation_.await_suspend(continuation);
    }
    auto await_resume() -> expected<std::pair<Stream, Addr>> {
      // 接受完成生成唯一原生拥有者；重复取结果绝不能为同一个 fd
      // 创建第二个关闭责任。
      if (std::exchange(consumed_, true))
        return std::unexpected{make_error(EINVAL)};
      auto result = operation_.await_resume();
      if (!result)
        return std::unexpected{result.error()};
      if constexpr (requires { address_.set_length(length_); })
        address_.set_length(length_);
      // 新 fd 尚未绑定任何 IO domain，直接注册到目标；禁止先绑定 listener
      // 再迁移。
      return adopt_accepted(owned_native_socket{*result}, address_, resource_,
                            context_, options_);
    }

  private:
    std::shared_ptr<io::detail::resource_state> resource_;
    Addr address_{};
    socklen_t length_{Addr::capacity()};
    io::io_context context_;
    accept_options options_;
    bool native_no_wait_{};
    decltype(io::accept(
        std::declval<std::shared_ptr<io::detail::resource_state>>(), nullptr,
        nullptr, 0)) operation_;
    void *owner_{};
    bool consumed_{};
  };
  /** @brief 接受一个连接，默认在同一 runtime 的活跃 IO domain 中均衡首次归属。
   */
  auto accept(accept_options options = {}) const noexcept {
    return Accept{resource(), context(), std::move(options)};
  }
  /** @brief 显式指定新连接归属；已发布 fd 不发生 domain 迁移。 */
  auto accept(io::io_context target) const noexcept {
    return accept(
        accept_options{accept_placement::explicit_context, std::move(target)});
  }
  /** @brief 同步接受一个已经完成握手的连接；EAGAIN 不挂起。 */
  [[nodiscard]] auto try_accept(accept_options options = {}) const
      -> expected<std::pair<Stream, Addr>> {
    return try_accept_impl(resource(), context(), options);
  }
  [[nodiscard]] auto try_accept(io::io_context target) const
      -> expected<std::pair<Stream, Addr>> {
    return try_accept(
        accept_options{accept_placement::explicit_context, std::move(target)});
  }
  /** @brief 有界接受批次，等待首条连接，其余只领取当前已排队的连接。
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
  auto accept_many(std::size_t maximum, io::io_context target) const
      -> task<expected<std::vector<std::pair<Stream, Addr>>>> {
    return accept_many(
        maximum,
        accept_options{accept_placement::explicit_context, std::move(target)});
  }
  auto close() noexcept { return socket_.close(); }
  [[nodiscard]] auto fd() const noexcept -> int { return socket_.fd(); }
  [[nodiscard]] auto as_native_handle() const noexcept -> int { return fd(); }
  [[nodiscard]] auto resource() const noexcept { return socket_.resource(); }
  [[nodiscard]] auto context() const noexcept { return socket_.context(); }
  auto ready(io::Interest interest = io::Interest::readable) const noexcept {
    return io::ready(resource(), interest);
  }
  auto readable() const noexcept { return ready(); }
  [[nodiscard]] auto into_native() -> expected<owned_native_socket> {
    return socket_.into_native();
  }
  [[nodiscard]] static auto from_native(io::io_context context,
                                        owned_native_socket native)
      -> expected<Listener> {
    int accepting{};
    socklen_t length = sizeof(accepting);
    if (::getsockopt(native.get(), SOL_SOCKET, SO_ACCEPTCONN, &accepting,
                     &length) < 0)
      return std::unexpected{make_error(errno)};
    if (!accepting)
      return std::unexpected{make_error(Error::InvalidSocketType)};
    if (auto valid = Socket::validate_family<Addr>(native.get()); !valid)
      return std::unexpected{valid.error()};
    if (auto result = Socket::prepare(native.get()); !result)
      return std::unexpected{result.error()};
    auto socket = Socket::adopt_checked(native.get(), std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return Listener{std::move(*socket)};
  }
  [[nodiscard]] static auto
  bind(const Addr &address, io::io_context context = io::io_context::current(),
       int backlog = SOMAXCONN) -> expected<Listener> {
    auto socket =
        Socket::create(address.family(), SOCK_STREAM, 0, std::move(context));
    if (!socket)
      return std::unexpected{socket.error()};
    if (auto result = socket->bind(address); !result)
      return std::unexpected{result.error()};
    if (auto result = socket->listen(backlog); !result)
      return std::unexpected{result.error()};
    return Listener{std::move(*socket)};
  }
  [[nodiscard]] static auto bind(io::io_context context, const Addr &address,
                                 int backlog = SOMAXCONN)
      -> expected<Listener> {
    return bind(address, std::move(context), backlog);
  }
  [[nodiscard]] static auto
  bind(std::span<const Addr> addresses,
       io::io_context context = io::io_context::current())
      -> expected<Listener> {
    Error last{Error::InvalidAddresses};
    for (const auto &address : addresses) {
      auto result = bind(address, context);
      if (result)
        return result;
      last = result.error();
    }
    return std::unexpected{last};
  }
  [[nodiscard]] static auto bind(io::io_context context,
                                 std::span<const Addr> addresses)
      -> expected<Listener> {
    return bind(addresses, std::move(context));
  }

private:
  /** @brief 根据 listener 最终归属选定目标，不把停机后的空分配结果当作 lazy
   * 资源。 */
  static auto
  select_context(const std::shared_ptr<io::detail::resource_state> &resource,
                 const io::io_context &context, const accept_options &options)
      -> expected<io::io_context> {
    // runtime 外创建的 listener 可能第一次 await 才绑定，不能使用旧空快照。
    const auto source =
        resource && resource->owner ? io::io_context{resource->owner} : context;
    switch (options.placement) {
    case accept_placement::listener_local:
      return source;
    case accept_placement::balanced: {
      // 同步、尚未绑定的原生 listener 保留第一次 IO 再绑定的兼容行为。
      if (!source)
        return source;
      auto target = source.balanced_context();
      if (!target)
        return std::unexpected{make_error(ECANCELED)};
      return target;
    }
    case accept_placement::explicit_context:
      if (!options.target)
        return std::unexpected{make_error(EINVAL)};
      return options.target;
    }
    return std::unexpected{make_error(EINVAL)};
  }
  /** @brief 唯一 guard 保留关闭责任，只有 target 注册成功才 release 到 stream。
   */
  static auto
  adopt_accepted(owned_native_socket native, Addr address,
                 const std::shared_ptr<io::detail::resource_state> &resource,
                 const io::io_context &context, const accept_options &options)
      -> expected<std::pair<Stream, Addr>> {
    if (auto prepared = Socket::prepare(native.get()); !prepared)
      return std::unexpected{prepared.error()};
    auto target = select_context(resource, context, options);
    if (!target)
      return std::unexpected{target.error()};
    auto socket = Socket::adopt_checked(native.get(), std::move(*target));
    if (!socket)
      return std::unexpected{socket.error()};
    (void)native.release();
    return std::pair{Stream{std::move(*socket)}, std::move(address)};
  }
  static auto
  try_accept_impl(const std::shared_ptr<io::detail::resource_state> &resource,
                  const io::io_context &context, const accept_options &options,
                  void *owner = nullptr) -> expected<std::pair<Stream, Addr>> {
    // 短 syscall 只持 source gate；释放 source 锁后注册 target，避免跨域 ABBA。
    auto accepted = io::detail::with_resource(
        resource, io::Interest::readable,
        [&]() -> expected<std::pair<owned_native_socket, Addr>> {
          Addr address{};
          socklen_t length = Addr::capacity();
#if defined(__linux__)
          const int descriptor =
              ::accept4(resource->fd(), address.sockaddr(), &length,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
          const int descriptor =
              ::accept(resource->fd(), address.sockaddr(), &length);
#endif
          if (descriptor < 0)
            return std::unexpected{make_error(errno)};
          if constexpr (requires { address.set_length(length); })
            address.set_length(length);
          return std::pair{owned_native_socket{descriptor}, std::move(address)};
        },
        owner);
    if (!accepted)
      return std::unexpected{accepted.error()};
    return adopt_accepted(std::move(accepted->first),
                          std::move(accepted->second), resource, context,
                          options);
  }
  /** @brief 批量 accept 保存调用时资源，并在整个批次保持读方向租约。 */
  static auto
  accept_many_impl(std::shared_ptr<io::detail::resource_state> resource,
                   io::io_context context, std::size_t maximum,
                   accept_options options)
      -> task<expected<std::vector<std::pair<Stream, Addr>>>> {
    if (!maximum)
      co_return std::unexpected{make_error(EINVAL)};
    char owner_identity{};
    auto reservation = io::detail::reserve_direction(
        resource, io::Interest::readable, &owner_identity);
    if (!reservation)
      co_return std::unexpected{reservation.error()};
    std::vector<std::pair<Stream, Addr>> accepted;
    accepted.reserve(maximum);
    auto first = co_await Accept{resource, context, options}.reservation(
        &owner_identity);
    if (!first)
      co_return std::unexpected{first.error()};
    accepted.push_back(std::move(*first));
    const bool native =
        resource->owner->supports_native(io::detail::operation_kind::accept);
    const bool native_no_wait =
        resource->owner->capabilities().native_accept_nowait;
    while (accepted.size() < maximum) {
      // 旧内核没有即时 ACCEPT 标志时合法返回已完成的首项；不偷偷同步 accept，
      // 也不为填满 maximum 等待下一条连接而破坏有界批次的即时返回合同。
      if (native && !native_no_wait)
        break;
      auto next =
          native
              ? co_await Accept{resource, context, options, true}.reservation(
                    &owner_identity)
              : try_accept_impl(resource, context, options, &owner_identity);
      if (!next) {
        if (next.error().value() == EAGAIN ||
            next.error().value() == EWOULDBLOCK)
          break;
        co_return std::unexpected{next.error()};
      }
      accepted.push_back(std::move(*next));
    }
    co_return accepted;
  }
  Socket socket_;
};
} // namespace faio::net::detail
#endif
