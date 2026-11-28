/**
 * @file faio/detail/io/platform/windows/domain.hpp
 * @brief 资源与操作状态域。
 */
#pragma once
#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/backend/factory.hpp"
#include "faio/detail/io/core/capabilities.hpp"
#include "faio/detail/io/core/config.hpp"
#include "faio/detail/io/core/context.hpp"
#include "faio/detail/io/core/operation_state.hpp"
#include "faio/detail/io/core/placement.hpp"
#include "faio/detail/io/core/session.hpp"
#include "faio/detail/io/core/request_rules.hpp"
#include "faio/detail/io/core/resource_state.hpp"
#include "faio/detail/io/platform/windows/request_operations.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <functional>
#include <limits>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace faio::io::detail {
/**
 * @brief 统一管理 Windows 资源身份、稳定操作槽、方向执行权及完成发布的域。
 * @details mutex_ 串行化接纳、取消和终态，driver_mutex_ 保证单一完成消费者。
 * IOCP 的原请求完成包解除原生借用；CancelIoEx 成功不能提前回收负载。
 * 文件辅助服务和不可取消清理独立记账，消费者在域锁外交付后才回收槽位。
 */
class io_domain : public std::enable_shared_from_this<io_domain> {
 public:
  /** @brief 构造 I/O 资源与操作的归属域。 */
  explicit io_domain(engine_config config)
      : backend_(
#if defined(FAIO_TEST_BACKEND_INJECTION)
            config.backend_factory ? config.backend_factory()
            : config.reactor_factory
                ? backend_box{std::make_unique<readiness_adapter>(config.reactor_factory())}
                :
#endif
                make_platform_backend(config.native_queue_entries)),
        max_resources_(config.max_resources),
        fs_(config.filesystem_service
                ? config.filesystem_service
                : std::make_shared<blocking::service>(config.filesystem_threads,
                                                      config.blocking_queue_limit,
                                                      1,
                                                      backend_.native()
                                                          ? blocking::startup_policy::ON_DEMAND
                                                          : blocking::startup_policy::PREHEATED)),
        dns_(config.resolver_service
                 ? config.resolver_service
                 : std::make_shared<blocking::service>(config.resolver_threads,
                                                       config.blocking_queue_limit,
                                                       1,
                                                       backend_.native()
                                                           ? blocking::startup_policy::ON_DEMAND
                                                           : blocking::startup_policy::PREHEATED)),
        cleanup_(config.cleanup_service
                     ? config.cleanup_service
                     : std::make_shared<blocking::service>(
                           1,
                           config.blocking_queue_limit,
                           1,
                           backend_.native() ? blocking::startup_policy::ON_DEMAND
                                             : blocking::startup_policy::PREHEATED)),
        own_fs_(!config.filesystem_service),
        own_dns_(!config.resolver_service),
        own_cleanup_(!config.cleanup_service),
        placement_(std::move(config.placement_service)) {
    if (!config.max_operations || !max_resources_ || config.max_operations >= UINT32_MAX
        || max_resources_ > SIZE_MAX - config.max_operations)
      throw std::invalid_argument("IO 容量无效或超出稳定token范围");
    operations_.reserve(config.max_operations);
    free_.reserve(config.max_operations);
    for (std::size_t i = 0; i < config.max_operations; ++i) {
      auto operation = std::make_unique<operation_state>();
      operation->slot = static_cast<std::uint32_t>(i);
      operations_.push_back(std::move(operation));
      free_.push_back(static_cast<std::uint32_t>(i));
    }
  }

  /** @brief 销毁 I/O 资源与操作的归属域。 */
  ~io_domain() {
    if (own_fs_)
      fs_->close();
    if (own_dns_)
      dns_->close();
    if (own_cleanup_)
      cleanup_->close();
  }

  /** @brief 禁止复制构造 I/O 资源与操作的归属域。 */
  io_domain(const io_domain&) = delete;

  /** @brief 读取当前组件是否已进入停止状态。 */
  bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

  /** @brief 取得所属任务或服务的协作停止令牌。 */
  std::stop_token stop_token() const noexcept { return stop_.get_token(); }

  /** @brief 取得文件或通用阻塞执行服务引用，具体服务归属由上下文决定。 */
  blocking::service_ref blocking() const noexcept { return blocking::service_ref{fs_}; }

  /** @brief 取得专门执行不可丢弃关闭工作的清理服务引用。 */
  blocking::service_ref cleanup() const noexcept { return blocking::service_ref{cleanup_}; }

  /** @brief 取得隔离地址解析阻塞调用的执行服务引用。 */
  blocking::service_ref resolver() const noexcept { return blocking::service_ref{dns_}; }

  /** @brief 返回实际已初始化后端提供的能力快照。 */
  io_capabilities capabilities() const noexcept {
    io_capabilities c;
    c.backend = backend_.name();
    c.native_filesystem = backend_.native() && supports_native(operation_kind::READ)
                          && supports_native(operation_kind::WRITE);
    c.zero_copy = supports_native(operation_kind::SEND_ZC);
    c.native_accept_nowait = supports_native(operation_kind::ACCEPT_NOWAIT);
    return c;
  }

  /** @brief 查询初始化时固定的原生能力，避免普通请求热路径重复探测平台。 */
  bool supports_native(operation_kind kind) const noexcept {
    return backend_.native() && backend_.supports(static_cast<std::uint32_t>(kind));
  }

  /** @brief 读取后端累计的提交、完成和控制事件统计。 */
  backend_statistics statistics() const noexcept { return backend_.statistics(); }

  /** @brief 通过独立控制通知唤醒底层等待，处理已发布的工作。 */
  void wake() const noexcept { backend_.wake(); }

  /**  @brief 外层 File 租约排空后注销原生缓存，关闭前防止数值重用命中旧注册。 */
  void forget_native_handle(native_descriptor handle, native_handle_kind kind) noexcept {
    std::lock_guard lock(mutex_);
    backend_.unregister_resource({static_cast<std::uintptr_t>(handle), kind});
  }

  /** @brief 从运行时放置服务选择 IO 域，未配置放置服务时沿用当前域。 */
  io_context balanced_context() noexcept {
    if (placement_)
      return io_context{placement_->select()};
    return stopped() ? io_context{} : io_context{shared_from_this()};
  }

  /**
   *  @brief 只读取本域已经登记的 fd 身份；查找不会执行系统调用或借用注册。
   * @details 弱引用提升与 handle 核对在域锁内完成，返回的租约持有原资源，使其保持有效。
   */
  resource_ptr find_registered_resource(native_descriptor fd) noexcept {
    std::lock_guard lock(mutex_);
    const auto found = descriptors_.find(fd);
    if (found == descriptors_.end())
      return {};
    auto resource = found->second.lock();
    return resource && resource->fd() == fd ? std::move(resource) : resource_ptr{};
  }

  /**
   * @brief 在兼容裸 fd 的低频路径中复用已有归属；携带 resource 的常规 I/O 不调用此接口。
   * @param include_runtime 是否允许查询本运行时的其他分片；显式 context 只查询本域。
   * @details 释放本域锁后才读取分片弱引用表，任何时候都不会同时持有这两种控制面锁。
   */
  resource_ptr find_raw_resource(native_descriptor fd, bool include_runtime) noexcept {
    if (auto resource = find_registered_resource(fd))
      return resource;
    return include_runtime && placement_ ? placement_->find_resource(fd, this) : resource_ptr{};
  }

  /**  @brief 注册长期资源；不接管失败的 fd，所有权仍归调用方。 */
  resource_ptr adopt(native_descriptor fd,
                     bool owns = true,
                     bool regular = false,
                     native_handle_kind kind = native_handle_kind::WINDOWS_SOCKET) {
    std::lock_guard lock(mutex_);
    if (stopped())
      throw std::system_error(ECANCELED, std::generic_category(), "IO domain 已停止");
    if (fd < 0)
      throw std::system_error(EBADF, std::generic_category());
    if (auto it = descriptors_.find(fd); it != descriptors_.end()) {
      if (auto old = it->second.lock()) {
        if (owns && old->owns_handle)
          throw std::system_error(EBUSY, std::generic_category(), "fd 已有拥有者");
        old->owns_handle |= owns;
        return old;
      }
    }
    if (resources_.size() >= max_resources_)
      throw std::system_error(ENFILE, std::generic_category());
    auto r = std::make_shared<resource_state>();
    r->owner = shared_from_this();
    r->id = ++next_resource_;
    if (!r->id)
      throw std::overflow_error("IO resource token exhausted");
    r->handle.store(fd, std::memory_order_release);
    r->regular_file = regular;
    r->native_kind = regular ? native_handle_kind::WINDOWS_HANDLE : kind;
    if (const int error =
            backend_.register_resource({static_cast<std::uintptr_t>(fd), r->native_kind}, r->id);
        error)
      throw std::system_error(-error, std::generic_category(), "IOCP register_resource");
    r->registered = true;
    r->socket_family =
        r->native_kind == native_handle_kind::WINDOWS_SOCKET ? query_socket_family(fd) : 0;
    resources_.emplace(r->id, r);
    descriptors_[fd] = r;
    r->owns_handle = owns;
    return r;
  }

  /** @brief 接管 OVERLAPPED 管道句柄，不启用文件游标或阻塞通道。 */
  resource_ptr adopt_pipe(native_descriptor fd) {
    return adopt(fd, true, false, native_handle_kind::WINDOWS_PIPE);
  }

  /** @brief 借用资源而不接管关闭责任。 */
  resource_ptr borrow(native_descriptor fd) {
    if (auto it = descriptors_.find(fd); it != descriptors_.end())
      if (auto resource = it->second.lock())
        return resource;
    u_long enabled = 1;
    if (::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &enabled) == SOCKET_ERROR)
      throw std::system_error(windows::encode_winsock_error(::WSAGetLastError()),
                              std::generic_category());
    return adopt(fd, false, false);
  }

  /**  @brief 原生导入对象首次挂起时绑定归属，保留原共享控制块。 */
  void bind(const resource_ptr& resource) {
    std::lock_guard binding_lock(resource->binding_mutex);
    std::lock_guard lock(mutex_);
    if (resource->owner) {
      if (resource->owner.get() != this)
        throw std::system_error(EXDEV, std::generic_category());
      return;
    }
    if (stopped())
      throw std::system_error(ECANCELED, std::generic_category());
    if (resources_.size() >= max_resources_)
      throw std::system_error(ENFILE, std::generic_category());
    const auto id = ++next_resource_;
    resources_.emplace(id, resource);
    try {
      descriptors_[resource->fd()] = resource;
    } catch (...) {
      resources_.erase(id);
      throw;
    }
    resource->id = id;
    if (const int error = backend_.register_resource(
            {static_cast<std::uintptr_t>(resource->fd()), resource->native_kind}, id);
        error) {
      resources_.erase(id);
      descriptors_.erase(resource->fd());
      throw std::system_error(-error, std::generic_category(), "IOCP register_resource");
    }
    resource->registered = true;
    resource->socket_family = query_socket_family(resource->fd());
    resource->owner = shared_from_this();
  }

  /** @brief 在资源状态锁内取得指定方向的组合操作预留。 */
  expected<void> reserve(resource_state& resource, interest interest, void* owner) noexcept {
    std::lock_guard lock(mutex_);
    const auto bits = static_cast<unsigned>(interest);
    if (!owner || !bits || bits > 3)
      return std::unexpected{make_error(EINVAL)};
    if (stopped() || resource.closing || resource.fd() < 0)
      return std::unexpected{make_error(EBADF)};
    if ((bits & 1 && (resource.reader || resource.read_reserved))
        || (bits & 2 && (resource.writer || resource.write_reserved)))
      return std::unexpected{make_error(EBUSY)};
    if (bits & 1)
      resource.read_reserved = owner;
    if (bits & 2)
      resource.write_reserved = owner;
    return {};
  }

  /** @brief 只释放由同一稳定身份持有的方向预留，并推进等待关闭的资源。 */
  void unreserve(resource_state& resource, interest interest, void* owner) noexcept {
    std::lock_guard lock(mutex_);
    const auto bits = static_cast<unsigned>(interest);
    if (bits & 1 && resource.read_reserved == owner)
      resource.read_reserved = nullptr;
    if (bits & 2 && resource.write_reserved == owner) {
      resource.write_reserved = nullptr;
      if (resource.write_shutdown && !resource.writer && resource.fd() >= 0)
        issue_shutdown_write(resource);
    }
  }

  /**  @brief 最后一个控制块退出；注销和 fd 接管都在同一个串行状态协议中完成。 */
  void release_resource(resource_state& resource) noexcept {
    std::lock_guard lock(mutex_);
    const native_descriptor fd = resource.handle.exchange(-1, std::memory_order_acq_rel);
    if (resource.registered && fd >= 0)
      backend_.unregister_resource({static_cast<std::uintptr_t>(fd), resource.native_kind});
    resources_.erase(resource.id);
    if (fd >= 0)
      descriptors_.erase(fd);
    if (resource.owns_handle && fd >= 0) {
      if (defer_native_close(fd))
        return;
      if (windows_operations::needs_deferred_close(resource, fd))
        defer_cleanup([fd, kind = resource.native_kind] {
          (void)windows_operations::close_native(fd, kind);
        });
      else
        (void)windows_operations::close_native(fd, resource.native_kind);
    }
  }

  /**  @brief 仅在无在途请求时导出所有权，所有 split 包装同时观察 detached。 */
  expected<native_descriptor> detach(resource_state& resource,
                                     bool preserve_association = false) noexcept {
    std::lock_guard lock(mutex_);
    if (resource.active || resource.close_waiter || resource.read_reserved
        || resource.write_reserved)
      return std::unexpected{make_error(EBUSY)};
    const native_descriptor fd = resource.handle.exchange(-1, std::memory_order_acq_rel);
    if (fd < 0)
      return std::unexpected{make_error(EBADF)};
    if (resource.registered && !preserve_association)
      backend_.unregister_resource({static_cast<std::uintptr_t>(fd), resource.native_kind});
    resource.registered = false;
    resource.closing = true;
    resource.owns_handle = false;
    resources_.erase(resource.id);
    descriptors_.erase(fd);
    return fd;
  }

  /**
   * @brief 在短锁内尝试非阻塞 I/O；完成后不留下内核、队列或消费者引用。
   * @details 仅用于单缓冲区的流或数据报 I/O；资源租约与方向门闩校验和正常 submit 相同。
   * EAGAIN 时回退到稳定的 prepare/submit；已注册且无新事件时，复用本次 EAGAIN。
   * 新注册或新的就绪代际仍须实际尝试系统调用，保证 ET 事件不丢失。
   * 普通文件不进入此路径；取消、close 与系统调用在同一锁内完成线性化。
   */
  std::optional<immediate_result> try_immediate(io_request& request,
                                                bool already_stopped) noexcept {
    return try_immediate_impl(request, already_stopped);
  }

  /**  @brief 紧凑 RECV/SEND 与完整请求共用资源锁、停止、deadline 和方向校验。 */
  std::optional<immediate_result> try_immediate(scalar_io_request& request,
                                                bool already_stopped) noexcept {
    return try_immediate_impl(request, already_stopped);
  }

 private:
  /** @brief 两种请求策略共用同一状态机，避免紧凑路径遗漏取消/关闭/方向规则。 */
  template <class Request>
  std::optional<immediate_result> try_immediate_impl(Request& request,
                                                     bool already_stopped) noexcept {
    if (backend_.native())
      return {};

    if constexpr (std::is_same_v<Request, scalar_io_request>) {
      if (request.kind != operation_kind::RECV && request.kind != operation_kind::SEND)
        return {};

    } else if (request.kind != operation_kind::RECV && request.kind != operation_kind::SEND
               && request.kind != operation_kind::READ && request.kind != operation_kind::WRITE) {
      return {};
    }
    const auto& resource = request.resource;
    if (!resource || resource->regular_file)
      return {};
    std::lock_guard lock(mutex_);
    if (already_stopped || stopped())
      return immediate_result{-ECANCELED, 0};
    if (request.validation_error)
      return immediate_result{-request.validation_error, 0};
    if (resource->owner.get() != this)
      return immediate_result{-EXDEV, 0};
    if (resource->closing || resource->fd() < 0)
      return immediate_result{-EBADF, 0};
    if (request.deadline && *request.deadline <= std::chrono::steady_clock::now())
      return immediate_result{-ETIMEDOUT, 0};
    const auto direction = direction_of(request.kind);
    if (request.establish_reservation
        && (!request.reservation || (direction != 1 && direction != 2)))
      return immediate_result{-EINVAL, 0};
    if ((direction & 1
         && (resource->reader
             || (resource->read_reserved && resource->read_reserved != request.reservation)))
        || (direction & 2
            && (resource->writer
                || (resource->write_reserved && resource->write_reserved != request.reservation))))
      return immediate_result{-EBUSY, 0};
    if (request.establish_reservation) {
      if (direction == 1)
        resource->read_reserved = request.reservation;
      else
        resource->write_reserved = request.reservation;
      request.establish_reservation = false;
    }
    request.fd = resource->fd();
    const auto result = [&] {
      if constexpr (std::is_same_v<Request, scalar_io_request>) {
        return windows_operations::perform_socket_scalar(request);
      } else {
        bool connecting = false;
        return windows_operations::perform(request, connecting);
      }
    }();
    if (result == -EAGAIN || result == -EWOULDBLOCK) {
      resource->readiness &= ~direction;
      request.observed_would_block = true;
      request.observed_readiness_generation = resource->readiness_generation;
      return {};
    }
    return immediate_result{result, result > 0 ? static_cast<std::uint64_t>(result) : 0};
  }

 public:
  /**
   * @brief 为桥接层取得带代际的槽位，准备阶段不执行 I/O。
   * @param request 持有路径、地址及 iovec 描述符的请求；调用者须保持数据缓冲区租约有效。
   * @param target 消费者的无异常完成入口，内核不会保存该地址。
   * @param error 槽位容量不足时写入 EAGAIN；此时尚未接受请求。
   * @return 本次槽位的唯一代际 token；零值表示准备失败。
   * @details Prepared 槽会保留已收到的取消请求；submit 才取得资源方向并执行系统调用。
   */
  operation_token prepare(io_request&& request, completion_target target, int& error) noexcept {
    std::lock_guard lock(mutex_);
    return prepare_locked(std::move(request), target, error);
  }

  /**
   * @brief 兼容左值请求，复制一次持有的参数后进入相同的稳定槽准备流程。
   * @details 常规 awaiter 使用右值重载，无需创建中间请求；此入口保留原左值的所有权。
   * 和原按值入口一样，路径或 iovec 分配失败发生在准备之前，异常向调用方传播。
   */
  operation_token prepare(const io_request& request, completion_target target, int& error) {
    io_request owned = request;
    return prepare(std::move(owned), target, error);
  }

  /**
   * @brief 接受槽位并尝试一次非阻塞操作，最终状态由统一完成链交付。
   * @param token prepare 返回的 token；旧代际和重复提交均无副作用。
   * @details 在同一短锁内取得所有资源门闩；文件请求交给隔离执行器。
   * 同步完成也通过发布协议交付，协程桥根据门闩状态决定不挂起。
   */
  void submit(operation_token token) noexcept {
    bool wake_needed = false;
    bool native_completion_opportunity = false;
    {
      std::lock_guard lock(mutex_);
      if (!submit_locked(token, wake_needed, native_completion_opportunity))
        return;
    }
    finish_submission(token, wake_needed, native_completion_opportunity);
  }

  /**
   * @brief 协程桥在同一域锁内准备并接受稳定请求，省去中间一次解锁与加锁。
   * @param stop 仅在本次调用中观察停止状态，不保存借用的 token 地址，也不注册回调。
   * @param cancellable false 表示不订阅任务停止；已接管的 CLOSE 仍由 uncancellable 决定。
   * @details 槽位选择、代际、方向门闩和原生请求均复用独立 prepare/submit 的状态机。
   * 在 Prepared 之后、accepted 之前检查已保留的取消请求；发布、驱动和唤醒均在域锁外。
   * target 可能在本函数返回前完成，调用方须预先建立 arming 门闩并持有租约。
   */
  operation_token prepare_submit(io_request&& request,
                                 completion_target target,
                                 int& error,
                                 const std::stop_token& stop = {},
                                 bool cancellable = true) noexcept {
    operation_token token;
    bool wake_needed = false;
    bool native_completion_opportunity = false;
    {
      std::lock_guard lock(mutex_);
      token = prepare_locked(std::move(request), target, error);
      if (!token.value)
        return {};

      if (cancellable && stop.stop_requested()) {
        auto* operation = lookup(token);
        if (operation && !operation->request.uncancellable && !operation->cancellation)
          operation->cancellation = ECANCELED;
      }
      if (!submit_locked(token, wake_needed, native_completion_opportunity))
        std::terminate();
    }
    finish_submission(token, wake_needed, native_completion_opportunity);
    return token;
  }

  /**  @brief 仅按代际 token 查找取消目标；Prepared 阶段收到的取消也会持续保留。 */
  void request_cancel(operation_token token, cancel_reason reason = cancel_reason::USER) noexcept {
    {
      std::lock_guard lock(mutex_);
      if (auto* op = lookup(token); op && !op->terminal && !op->request.uncancellable) {
        if (!op->cancellation)
          op->cancellation = reason == cancel_reason::DEADLINE ? ETIMEDOUT : ECANCELED;
        if (op->accepted)
          cancel_operation(*op);
      }
    }
    wake();
  }

  /**  @brief 最后一个包装对象析构时请求 close，不等待完成，也不依赖当前线程 TLS。 */
  void request_close(const resource_ptr& resource) noexcept {
    {
      std::lock_guard lock(mutex_);
      begin_close(*resource, nullptr);
    }
    wake();
  }

  /**  @brief 请求写半关闭；单次写和组合写方向租约全部排空后才发 SHUT_WR。 */
  void request_shutdown_write(resource_state& resource) noexcept {
    std::lock_guard lock(mutex_);
    resource.write_shutdown = true;
    if (!resource.writer && !resource.write_reserved && resource.fd() >= 0)
      issue_shutdown_write(resource);
  }

  /**  @brief Windows 没有异步原生 close opcode；关闭责任由独立清理 lane 接管。 */
  bool defer_native_close(native_descriptor, ::faio::move_only_function<void(int)> = {}) noexcept {
    return false;
  }

  /**  @brief 按预算公平推进驱动；批量内核事件、取消、deadline 和完成共用一套状态协议。 */
  drive_result drive(drive_budget budget = {}, std::optional<int> wait = 0) noexcept {
    ::faio::detail::scoped_thread_context cleanup_context;
    if (::faio::detail::current_thread_context().io.driver_domain)
      return {0, false, false, EBUSY};
    std::unique_lock driver_lock(driver_mutex_, std::try_to_lock);
    if (!driver_lock.owns_lock())
      return {0, false, false, EBUSY};
    driver_session_guard session{this};
    std::array<backend_event, 256> events;
    {
      std::lock_guard lock(mutex_);
      pump_native_pending();
      expire_deadlines();
      if (completed_head_)
        wait = 0;
      if (next_deadline_) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                                   *next_deadline_ - std::chrono::steady_clock::now())
                                   .count();
        const int ms = static_cast<int>(std::clamp<std::int64_t>(remaining, 0, INT_MAX));
        if (!wait || ms < *wait)
          wait = ms;
      }
    }
    int flush_error{};
    if (!backend_.poll_flushes_submissions()) {
      std::lock_guard lock(mutex_);
      if (!fatal_error_)
        flush_error = backend_.flush().error;
    }
    if (flush_error)
      enter_failure(flush_error);
    const int count = backend_.poll(
        std::span(events).first(std::clamp<std::size_t>(budget.max_events, 1, events.size())),
        wait);
    if (count < 0) {
      enter_failure(-count);
      std::lock_guard lock(mutex_);
      for (auto& entry : operations_)
        if (entry->allocated && entry->accepted && !entry->terminal && !entry->running_file
            && !entry->native_inflight && !entry->request.uncancellable)
          finish(*entry, count);
    } else {
      std::lock_guard lock(mutex_);
      for (int i = 0; i < count; ++i) {
        const auto event = events[static_cast<std::size_t>(i)];
        if (!event.key)
          continue;
        if (event.kind != backend_event_kind::READINESS) {
          complete_native(event);
          continue;
        }
        auto it = resources_.find(event.key);
        if (it == resources_.end())
          continue;
        auto resource = it->second.lock();
        if (!resource || resource->closing)
          continue;
        ++resource->readiness_generation;

        resource->readiness |= event.flags;
        if (resource->reader && event.flags & (READABLE_BIT | ERROR_BIT))
          attempt(*resource->reader);
        if (resource->writer && event.flags & (WRITABLE_BIT | ERROR_BIT))
          attempt(*resource->writer);
        auto* observer = resource->observers;
        while (observer) {
          auto* next = observer->observer_next;
          attempt(*observer);
          observer = next;
        }
      }
      expire_deadlines();
    }
    const auto published = publish_completed(std::max<std::size_t>(budget.max_completions, 1));
    bool more;
    {
      std::lock_guard lock(mutex_);
      pump_native_pending();
      more = completed_head_ != nullptr || !native_pending_.empty();
      if (free_.size() + retired_slots_ == operations_.size() && native_controls_.empty()
          && publishers_.load(std::memory_order_acquire) == 0 && backend_.quiescent())
        quiescent_cv_.notify_all();
    }
    return {published, published != 0 || count > 0, more, fatal_error_};
  }

  /** @brief 计算到最近截止时间的非负毫秒数，作为底层等待上限。 */
  std::optional<int> next_deadline() noexcept {
    std::lock_guard lock(mutex_);
    if (!next_deadline_)
      return {};
    const auto duration = std::chrono::ceil<std::chrono::milliseconds>(
                              *next_deadline_ - std::chrono::steady_clock::now())
                              .count();
    return static_cast<int>(std::clamp<std::int64_t>(duration, 0, INT_MAX));
  }

  /** @brief 检查执行、内核引用和完成责任是否均已归还。 */
  bool quiescent() const noexcept {
    std::lock_guard lock(mutex_);
    return free_.size() + retired_slots_ == operations_.size()
           && publishers_.load(std::memory_order_acquire) == 0 && native_controls_.empty()
           && backend_.quiescent();
  }

  /**  @brief 控制线程等待驱动器排空，避免 CPU 忙轮询，也不争用轮询会话。 */
  void wait_quiescent() noexcept {
    std::unique_lock lock(mutex_);
    quiescent_cv_.wait(lock, [&] {
      return free_.size() + retired_slots_ == operations_.size()
             && publishers_.load(std::memory_order_acquire) == 0 && native_controls_.empty()
             && backend_.quiescent();
    });
  }

  /**  @brief 停机先拒绝提交，随后取消、关闭、排空；清理通道保持可用。 */
  void begin_shutdown(shutdown_policy policy = shutdown_policy::CANCEL_ALL) noexcept {
    if (stopped_.exchange(true, std::memory_order_acq_rel))
      return;
    if (policy == shutdown_policy::CANCEL_ALL)
      stop_.request_stop();

    std::unordered_map<std::uint64_t, ::faio::move_only_function<void()>> cleanups;
    {
      std::lock_guard lock(mutex_);
      backend_.begin_shutdown();
      draining_ = policy == shutdown_policy::DRAIN;
      cleanups.swap(shutdown_cleanups_);
      if (policy == shutdown_policy::CANCEL_ALL) {
        for (auto& entry : operations_)
          if (entry->allocated && !entry->terminal && !entry->request.uncancellable) {
            if (!entry->cancellation)
              entry->cancellation = ECANCELED;
            cancel_operation(*entry);
          }
      }
      for (auto it = resources_.begin(); it != resources_.end();) {
        auto weak = (it++)->second;
        if (auto resource = weak.lock();
            resource && (policy == shutdown_policy::CANCEL_ALL || !resource->active))
          begin_close(*resource, nullptr);
      }
    }
    for (auto& [token, cleanup] : cleanups) {
      try {
        cleanup();
      } catch (...) {
        std::terminate();
      }
    }
    wake();
  }

  /** @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。 */
  void shutdown() noexcept {
    ::faio::detail::scoped_thread_context cleanup_context;
    begin_shutdown();
    while (!quiescent())
      (void)drive({}, 10);
    if (own_fs_)
      fs_->close();
    if (own_dns_)
      dns_->close();
    if (own_cleanup_)
      cleanup_->close();
  }

  /** @brief 登记持有自身状态的停机清理回调，使服务关闭时仍能归还长期资源。 */
  std::uint64_t register_shutdown_cleanup(::faio::move_only_function<void()> callback) {
    std::uint64_t token{};
    {
      std::lock_guard lock(mutex_);
      if (!stopped()) {
        token = ++next_cleanup_;
        if (!token)
          throw std::overflow_error("shutdown cleanup token exhausted");
        shutdown_cleanups_.emplace(token, std::move(callback));
      }
    }
    if (!token)
      callback();
    return token;
  }

  /** @brief 撤销已经由正常关闭路径接管的停机清理责任。 */
  void unregister_shutdown_cleanup(std::uint64_t token) noexcept {
    std::lock_guard lock(mutex_);
    shutdown_cleanups_.erase(token);
  }

  /**
   *  @brief 仅登记停机排空责任；等待外层租约释放时不创建线程、任务或请求。
   * @details File/ReadDir 的唯一关闭者取得票据，在最终 CLOSE/closedir 后释放。
   */
  void retain_cleanup_wait() noexcept {
    if (publishers_.fetch_add(1, std::memory_order_acq_rel) == SIZE_MAX)
      std::terminate();
  }

  /** @brief 归还配对的清理等待责任，并在最后责任解除时通知排空方。 */
  void release_cleanup_wait() noexcept {
    const auto previous = publishers_.fetch_sub(1, std::memory_order_acq_rel);
    if (!previous)
      std::terminate();
    if (previous == 1)
      quiescent_cv_.notify_all();
    wake();
  }

  /**  @brief 清理任务交由专属通道执行；通道满载时保存在清理链中，不阻塞 worker。 */
  void defer_cleanup(::faio::move_only_function<void()> function) noexcept {
    try {
      auto job = std::make_shared<::faio::move_only_function<void()>>(std::move(function));
      auto self = shared_from_this();
      publishers_.fetch_add(1, std::memory_order_release);
      auto work = [self, job] {
        (*job)();
        if (self->publishers_.fetch_sub(1, std::memory_order_acq_rel) == 1)
          self->quiescent_cv_.notify_all();
        self->wake();
      };
      if (auto result = cleanup_->try_submit(std::move(work)); !result) {
        std::lock_guard lock(mutex_);
        deferred_cleanup_.push_back(std::move(job));
        wake();
      }
    } catch (...) {
      std::terminate();
    }
  }

  /** @brief 读取资源就绪位与观察代际，供就绪守卫安全消费提示。 */
  std::pair<readiness, std::uint64_t> snapshot_readiness(
      const resource_state& resource) const noexcept {
    std::lock_guard lock(mutex_);
    return {readiness{resource.readiness, resource.readiness_generation},
            resource.readiness_generation};
  }

  /** @brief 仅清除当前观察代际的指定就绪方向，不能抹掉后来收到的新事件。 */
  void clear_readiness(resource_state& resource,
                       interest interest,
                       std::uint64_t generation) noexcept {
    std::lock_guard lock(mutex_);
    if (resource.readiness_generation == generation)
      resource.readiness &= ~static_cast<std::uint32_t>(interest);
  }

  /** @brief 读取资源就绪位与观察代际，供就绪守卫安全消费提示。 */
  std::pair<readiness, std::uint64_t> snapshot_readiness(
      const resource_ptr& resource) const noexcept {
    std::lock_guard lock(mutex_);
    return {readiness{resource ? resource->readiness : 0,
                      resource ? resource->readiness_generation : 0},
            resource ? resource->readiness_generation : 0};
  }

  /**  @brief 只清除同一代际的观察结果，保留 guard 之后到达的新事件。 */
  void clear_readiness(const resource_ptr& resource,
                       interest interest,
                       std::uint64_t generation) noexcept {
    std::lock_guard lock(mutex_);
    if (resource && resource->readiness_generation == generation)
      resource->readiness &= ~static_cast<std::uint32_t>(interest);
  }

  /**  @brief try_* 与异步方向执行权共用同一 gate，EAGAIN 清除相应缓存。 */
  template <class F>
  auto with_resource(const resource_ptr& resource,
                     interest interest,
                     F&& function,
                     void* reservation = nullptr) -> std::invoke_result_t<F> {
    using result_type = std::invoke_result_t<F>;
    std::lock_guard lock(mutex_);
    const auto bits = static_cast<std::uint32_t>(interest);
    if (bits > 3)
      return result_type{std::unexpected{make_error(EINVAL)}};
    if (stopped() || resource->closing || resource->fd() < 0)
      return result_type{std::unexpected{make_error(EBADF)}};
    if ((bits & 1
         && (resource->reader
             || (resource->read_reserved && resource->read_reserved != reservation)))
        || (bits & 2
            && (resource->writer
                || (resource->write_reserved && resource->write_reserved != reservation))))
      return result_type{std::unexpected{make_error(EBUSY)}};
    auto result = std::invoke(std::forward<F>(function));
    if (!result
        && (result.error().value() == EAGAIN || result.error().value() == EWOULDBLOCK
            || (result.error().domain() == error_domain::WINSOCK
                && result.error().value() == WSAEWOULDBLOCK)))
      resource->readiness &= ~bits;
    return result;
  }

 private:
  /**
   *  @brief 从完成链中独占领取的当前 token 结果快照；回调返回前，稳定槽仍保持已分配状态。
   * @details 仅复制借用目标和标量结果，不移动 request 或 resource，
   * 也不提前允许下一代复用槽位。
   */
  struct claimed_completion {
    operation_state* operation{}; /**< 空指针表示尚未领取；没有新完成或回收责任。 */
    completion_target target{};   /**< 在回调前复制；发布后不再访问消费者地址。 */
    std::int64_t result{};        /**< 最终原生完成包或统一 finish 流程产生的业务结果。 */
    std::uint64_t transferred{};  /**< 统一状态机保存的实际进度。 */
  };

  /**  @brief 调用者已持有域锁时，仅准备槽位；不取得执行权、不提交 IO、不发布消费者。 */
  operation_token prepare_locked(io_request&& request,
                                 completion_target target,
                                 int& error) noexcept {
    operation_state* selected = nullptr;
    while (!free_.empty()) {
      const auto candidate_index = free_.back();
      free_.pop_back();
      auto& candidate = *operations_[candidate_index];
      if (candidate.generation == UINT32_MAX) {
        ++retired_slots_;
        continue;
      }
      selected = &candidate;
      break;
    }
    if (!selected) {
      if (backend_.native()
          && (request.kind == operation_kind::CLOSE || request.internal_control)) {
        try {
          if (native_controls_.size() >= max_resources_ + operations_.size()
              || next_control_ == UINT32_MAX)
            std::terminate();
          const auto token = (static_cast<std::uint64_t>(++next_control_) << 32) | UINT32_MAX;
          auto owned = std::make_unique<operation_state>();
          owned->slot = UINT32_MAX;
          selected = native_controls_.emplace(token, std::move(owned)).first->second.get();
          selected->token = {token};
        } catch (...) {
          std::terminate();
        }
      }
    }
    if (!selected) {
      error = EAGAIN;
      if (free_.size() + retired_slots_ == operations_.size())
        quiescent_cv_.notify_all();
      return {};
    }
    auto& op = *selected;
    const auto index = op.slot;
    if (index != UINT32_MAX)
      ++op.generation;
    op.request = std::move(request);
    op.target = target;
    if (index != UINT32_MAX)
      op.token = {static_cast<std::uint64_t>(op.generation) << 32 | (index + 1)};
    op.allocated = true;
    op.accepted = false;
    op.terminal = false;
    op.running_file = false;
    op.native_inflight = false;
    op.native_pending = false;
    op.native_cancel_requested = false;
    op.native_waiting_release = false;
    op.native_close = false;
    op.counts_active = false;
    op.connecting = false;
    op.uses_reader = false;
    op.uses_writer = false;
    op.observer = false;
    op.queued_completion = false;
    op.completed_previous = nullptr;
    op.result = 0;
    op.transferred = 0;
    op.cancellation = 0;
    op.next = nullptr;
    op.observer_next = nullptr;
    return op.token;
  }

  /**
   * @brief 在调用者已持有域锁时接受请求，与独立 submit 使用相同的状态转换。
   * @return false 表示旧代际或已接受的 token；true 表示本次接管了请求完成责任。
   * @details 参数错误、截止时间、关闭、方向预留、原生提交和背压均由此入口处理。
   */
  bool submit_locked(operation_token token,
                     bool& wake_needed,
                     bool& native_completion_opportunity) noexcept {
    auto* op = lookup(token);
    if (!op || op->accepted)
      return false;
    op->accepted = true;
    if (stopped() && op->request.kind != operation_kind::CLOSE && !op->request.uncancellable)
      finish(*op, -ECANCELED);
    else if (op->cancellation)
      finish(*op, -op->cancellation);
    else if (op->request.validation_error)
      finish(*op, -op->request.validation_error);

    else if (op->request.kind == operation_kind::READY
             && (op->request.argument <= 0 || op->request.argument > 3))
      finish(*op, -EINVAL);

    else if (op->request.kind == operation_kind::RECVMSG && !op->request.output_message)
      finish(*op, -EFAULT);
    else if (op->request.kind == operation_kind::RECVMSG
             && static_cast<std::uint64_t>(op->request.output_message->msg_iovlen)
                    > native_iov_limit)
      finish(*op, -EINVAL);
    else if (op->request.kind == operation_kind::RECVMSG && op->request.output_message->msg_iovlen
             && !op->request.output_message->msg_iov)
      finish(*op, -EFAULT);
    else if (op->request.kind == operation_kind::CLOSE && !op->request.resource
             && op->request.fd < 0)
      finish(*op, -EBADF);
    else {
      try {
        auto& req = op->request;
        if (!req.bypass_resource_registration && !req.resource && req.fd >= 0
            && req.kind != operation_kind::OPEN && req.kind != operation_kind::OPEN2
            && req.kind != operation_kind::SOCKET)
          req.resource = borrow(req.fd);
        const auto& r = req.resource;
        if (r && r->owner.get() != this)
          finish(*op, -EXDEV);
        else if (r && req.kind != operation_kind::CLOSE && (r->fd() < 0 || r->closing))
          finish(*op, -EBADF);
        else if (req.establish_reservation && !r)
          finish(*op, -EBADF);
        else {
          if (r)
            req.fd = r->fd();
          if (r)
            req.native_kind = r->native_kind;
          if (req.deadline && *req.deadline <= std::chrono::steady_clock::now())
            finish(*op, -ETIMEDOUT);
          else if (req.kind == operation_kind::CLOSE && r) {
            r->owns_handle = true;
            begin_close(*r, op);
          } else {
            if (r) {
              const auto direction = direction_of(req.kind);
              if (req.establish_reservation
                  && (!req.reservation || (direction != 1 && direction != 2)))
                finish(*op, -EINVAL);

              else if ((direction & 1
                        && (r->reader || (r->read_reserved && r->read_reserved != req.reservation)))
                       || (direction & 2
                           && (r->writer
                               || (r->write_reserved && r->write_reserved != req.reservation))))
                finish(*op, -EBUSY);
              else {
                if (req.establish_reservation) {
                  if (direction == 1)
                    r->read_reserved = req.reservation;
                  else
                    r->write_reserved = req.reservation;
                  req.establish_reservation = false;
                }
                if (direction & 1) {
                  r->reader = op;
                  op->uses_reader = true;
                }
                if (direction & 2) {
                  r->writer = op;
                  op->uses_writer = true;
                }
                ++r->active;

                op->counts_active = true;
                if (req.kind == operation_kind::READY) {
                  op->observer = true;
                  op->observer_next = r->observers;
                  r->observers = op;
                }
              }
            }
            if (!op->terminal) {
              if (req.deadline)
                update_deadline(*req.deadline);
              if (can_native_request(req))
                submit_native(*op);
              else if (windows_operations::is_file_request(req))
                submit_file(*op);
              else if (req.observed_would_block && r && r->registered
                       && req.observed_readiness_generation == r->readiness_generation
                       && !(r->readiness & (direction_of(req.kind) | ERROR_BIT))) {
              } else
                attempt(*op);
            }
          }
        }
      } catch (const std::system_error& e) {
        finish(*op, -e.code().value());
      } catch (const std::bad_alloc&) {
        finish(*op, -ENOMEM);
      } catch (...) {
        finish(*op, -EIO);
      }
    }
    const auto* thread = ::faio::detail::try_current_thread_context();
    if (!thread || thread->io.domain != this) {
      const auto* pending = lookup(token);
      const bool other_completion =
          completed_head_ && (completed_head_->token.value != token.value || completed_head_->next);
      if ((pending && !pending->terminal) || other_completion)
        wake_needed = true;
    }
    native_completion_opportunity = thread && thread->io.domain == this && !thread->io.driver_domain
                                    && backend_.native() && can_complete_local_native(token);
    return true;
  }

  /**
   *  @brief 接受请求后，在锁外尝试低负载下的原生推进、即时交付与远程控制通知。
   * @details 仅重用既有的原生提交/完成
   * 和发布协议；不创建子协程、辅助队列或工作线程。
   */
  void finish_submission(operation_token token,
                         bool wake_needed,
                         bool native_completion_opportunity) noexcept {
    const auto delivery =
        native_completion_opportunity ? try_complete_local_native(token) : claimed_completion{};
    if (delivery.operation)
      publish_claimed(delivery);
    else
      publish_one(token);
    if (wake_needed)
      wake();
  }

  /**  @brief 只在首次注册读取 socket family；普通文件/pipe 返回 AF_UNSPEC。 */
  static int query_socket_family(native_descriptor fd) noexcept {
    int family = AF_UNSPEC;
#if defined(SO_DOMAIN)
    socklen_t length = sizeof(family);
    if (::getsockopt(static_cast<SOCKET>(fd),
                     SOL_SOCKET,
                     SO_DOMAIN,
                     reinterpret_cast<char*>(&family),
                     &length)
        < 0)
      return AF_UNSPEC;
#else
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getsockname(static_cast<SOCKET>(fd), reinterpret_cast<sockaddr*>(&address), &length) < 0)
      return AF_UNSPEC;
    family = address.ss_family;
#endif
    return family;
  }

  /**  @brief 写方向与组合租约全部排空后只执行一次 Winsock 半关闭。 */
  void issue_shutdown_write(resource_state& resource) noexcept {
    if (resource.write_shutdown_issued || resource.closing || resource.fd() < 0)
      return;
    resource.write_shutdown_issued = true;
    (void)::shutdown(static_cast<SOCKET>(resource.fd()), SD_SEND);
  }

  /**  @brief native 能力固定，零拷贝接口使用等价 OVERLAPPED 发送而不宣称零拷贝。 */
  bool can_native_request(const io_request& request) const noexcept {
    if (request.native_kind == native_handle_kind::WINDOWS_HANDLE && request.windows_implicit_cursor
        && ::GetFileType(reinterpret_cast<HANDLE>(request.fd)) == FILE_TYPE_DISK)
      return false;

    return request.kind == operation_kind::SEND_ZC || request.kind == operation_kind::SENDMSG_ZC
           || supports_native(request.kind);
  }

  /**
   *  @brief 在域锁内判断能否尝试低负载原生推进，不扫描操作槽，也不增加热路径计数器。
   * @details listener ACCEPT 与一条连接可共用域；至多八个已分配槽和资源才触发。
   *          资源数门控保守保留多连接批处理，已有完成/控制/背压优先交给正常
   * driver。
   */
  bool can_complete_local_native(operation_token token) noexcept {
    const auto allocated = operations_.size() - free_.size() - retired_slots_;
    if (allocated > 8 || resources_.size() > 8 || !native_controls_.empty()
        || !native_pending_.empty() || completed_head_ || fatal_error_ || stopped())
      return false;
    auto* operation = lookup(token);
    return operation && operation->accepted && operation->native_inflight && !operation->terminal
           && !operation->request.internal_control && static_cast<bool>(operation->target);
  }

  /**
   *  @brief 本地低负载时尝试一次原生提交和完成消费；不等待，也不执行同步业务系统调用。
   * @param token submit 当前操作的完整代际，完成后仍由其原 publish_one 交付。
   * @details driver mutex 只 try_lock，未取得或不再符合条件即保持原挂起行为。
   *          poll(0) 内部已经 flush；禁止另做重复 flush 或循环等待完成包。
   *          ACK/MORE/NOTIF 全部复用 complete_native 的租约与唯一终态协议。
   */
  claimed_completion try_complete_local_native(operation_token token) noexcept {
    const auto* thread = ::faio::detail::try_current_thread_context();
    if (!thread || thread->io.domain != this || thread->io.driver_domain || !backend_.native())
      return {};

    std::unique_lock driver_lock(driver_mutex_, std::try_to_lock);
    if (!driver_lock.owns_lock())
      return {};
    driver_session_guard session{this};
    int failure{};
    claimed_completion delivery;
    {
      std::lock_guard lock(mutex_);
      if (!can_complete_local_native(token))
        return {};
      std::array<backend_event, 8> events;
      const int count = backend_.poll(events, 0);
      if (count < 0)
        failure = -count;

      else {
        for (int index = 0; index < count; ++index)
          complete_native(events[static_cast<std::size_t>(index)]);
        delivery = claim_completed_locked(token);
      }
    }
    if (failure)
      enter_failure(failure);
    return delivery;
  }

  /**
   * @brief 后端永久故障时，须先确认内核引用已排空，才能将仍在途的缓冲区发布为失败结果。
   */
  void enter_failure(int error) noexcept {
    {
      std::lock_guard lock(mutex_);
      if (fatal_error_)
        return;
      fatal_error_ = error;
      if (!backend_.begin_failure(error))
        std::terminate();

      for (auto& entry : operations_)
        if (entry->allocated && entry->accepted && !entry->terminal
            && !entry->request.uncancellable)
          if (!entry->cancellation)
            entry->cancellation = error;
    }
    begin_shutdown();
  }

  /**
   *  @brief native accepted 与内核引用分离；稳定池满不会丢请求，也不在 worker
   * 阻塞。
   * @details pending 链最多操作池容量；请求始终在槽内，路径、iovec
   * 描述符地址稳定。
   */
  void submit_native(operation_state& op) noexcept {
    if (empty_stream_request(op.request)) {
      finish(op, 0);
      return;
    }
    const auto result = fatal_error_
                            ? backend_submit_result{backend_submit_status::REJECTED, fatal_error_}
                            : backend_.try_submit({op.token.value, &op.request});
    if (result.status == backend_submit_status::ACCEPTED) {
      op.native_inflight = true;
      op.native_pending = false;
    } else if (result.status == backend_submit_status::WOULD_QUEUE
               || ((op.request.kind == operation_kind::CLOSE || op.request.internal_control)
                   && result.error == ENOMEM)) {
      if (!op.native_pending) {
        op.native_pending = true;
        try {
          native_pending_.push_back(op.token);
        } catch (...) {
          op.native_pending = false;
          if (op.request.kind == operation_kind::CLOSE || op.request.internal_control)
            std::terminate();
          finish(op, -ENOMEM);
        }
      }
    } else {
      op.native_pending = false;
      if (op.request.kind == operation_kind::CLOSE) {
        std::terminate();
      } else
        finish(op, -result.error);
    }
  }

  /**  @brief 按预算重试一批请求；完成包消费后释放稳定节点容量，再把排队请求交给后端。 */
  void pump_native_pending() noexcept {
    const auto limit = std::min<std::size_t>(native_pending_.size(), 256);
    for (std::size_t i = 0; i < limit; ++i) {
      const auto token = native_pending_.front();
      native_pending_.pop_front();
      auto* op = lookup(token);
      if (!op || op->terminal || !op->native_pending)
        continue;
      op->native_pending = false;
      if (op->cancellation && !op->native_close && !op->request.uncancellable)
        finish(*op, -op->cancellation);
      else
        submit_native(*op);
    }
  }

  /**
   *  @brief 取消 ACK 不解除 payload 租约；这里只要求内核取消，原完成包
   * 决定终态。
   */
  void cancel_operation(operation_state& op) noexcept {
    if (op.native_close || op.request.uncancellable)
      return;
    if (op.native_inflight) {
      if (!op.native_cancel_requested && !op.native_close) {
        op.native_cancel_requested = true;
        backend_.request_cancel({op.token.value, &op.request});
      }
    } else if (!op.running_file) {
      op.native_pending = false;
      finish(op, -op.cancellation);
    }
  }

  /**  @brief 原生结果与资源就绪在核心汇合；MORE 结果保留到 NOTIF 后才发布一次。 */
  void complete_native(const backend_event& event) noexcept {
    if (event.kind == backend_event_kind::CANCEL_ACK)
      return;
    auto* op = lookup(operation_token{event.key});
    if (!op || !op->native_inflight || op->terminal)
      return;
    if (event.kind == backend_event_kind::BUFFER_RELEASE) {
      if (!op->native_waiting_release)
        return;
      op->native_waiting_release = false;
      op->native_inflight = false;
      finish(*op, op->result);
      return;
    }
    op->result = event.result;
    if (event.flags & 1u) {
      op->native_waiting_release = true;
      return;
    }
    op->native_inflight = false;
    if (op->native_close) {
      auto resource = op->request.resource;
      if (resource) {
        resource->close_pending = false;
        finish_close_waiters(*resource, event.result < 0 ? static_cast<int>(-event.result) : 0);
      }
      if (!op->terminal)
        finish(*op, event.result);
      return;
    }
    if (op->request.kind == operation_kind::READY && event.result >= 0 && op->request.resource) {
      auto& resource = *op->request.resource;
      std::uint32_t flags{};
      if (event.result & (POLLIN | POLLPRI))
        flags |= READABLE_BIT;
      if (event.result & POLLOUT)
        flags |= WRITABLE_BIT;
      if (event.result & (POLLERR | POLLNVAL))
        flags |= ERROR_BIT;
      if (event.result & POLLHUP)
        flags |= READABLE_BIT | WRITABLE_BIT | CLOSED_BIT;
#ifdef POLLRDHUP
      if (event.result & POLLRDHUP)
        flags |= READABLE_BIT | READ_CLOSED_BIT;
#endif
      ++resource.readiness_generation;
      resource.readiness |= flags;
      finish(*op, flags);
    } else {
      finish(*op, event.result);
    }
  }

  /** @brief 同时核对槽位、分配状态和完整代际；旧令牌不能访问复用后的操作。 */
  operation_state* lookup(operation_token token) noexcept {
    const auto index = static_cast<std::uint32_t>(token.value);
    if (index == UINT32_MAX) {
      const auto found = native_controls_.find(token.value);
      return found == native_controls_.end() ? nullptr : found->second.get();
    }
    if (!index || index > operations_.size())
      return nullptr;
    auto* op = operations_[index - 1].get();
    return op->allocated && op->token == token ? op : nullptr;
  }

  /**  @brief 只有操作租约排空后才执行一次对应种类的原生关闭器。 */

  /**
   *  @brief IOCP 已实现的请求不经过此处；短控制与平台 fallback 在明确的 lane
   * 执行。
   */

  /**
   * @brief 每个资源仅建立一次长期就绪注册，避免每次操作都执行 MOD 或重新布防。
   */
  int ensure_registration(resource_state& resource) noexcept {
    if (resource.registered)
      return 0;
    if (resource.regular_file)
      return -ENOTSUP;
    const int result = backend_.register_resource(
        {static_cast<std::uintptr_t>(resource.fd()), resource.native_kind}, resource.id);
    if (!result)
      resource.registered = true;
    return result;
  }

  /**
   * @brief 就绪适配器先尝试实际 I/O，仅在返回 EAGAIN 时继续保持挂起。
   */
  void attempt(operation_state& op) noexcept {
    if (op.terminal || op.running_file || op.native_inflight)
      return;
    const auto& resource = op.request.resource;
    if (op.cancellation) {
      finish(op, -op.cancellation);
      return;
    }
    if (op.request.kind == operation_kind::READY) {
      const auto mask = static_cast<std::uint32_t>(op.request.argument);
      if (!mask || mask > 3 || !resource) {
        finish(op, -EINVAL);
        return;
      }
      if (const int e = ensure_registration(*resource); e) {
        finish(op, e);
        return;
      }
      if (resource->readiness & (mask | ERROR_BIT))
        finish(op, resource->readiness);
      return;
    }
    if (op.request.kind == operation_kind::CANCEL) {
      if (static_cast<unsigned>(op.request.flags) & ~3u) {
        finish(op, -ENOTSUP);

        return;
      }
      if (resource) {
        for (auto& entry : operations_)
          if (entry.get() != &op && entry->allocated && entry->accepted && !entry->terminal
              && (entry->request.resource == resource
                  || (entry->request.bypass_resource_registration && !entry->request.resource
                      && entry->request.fd == op.request.fd))
              && entry->request.kind != operation_kind::CLOSE && !entry->request.uncancellable) {
            if (!entry->cancellation)
              entry->cancellation = ECANCELED;
            cancel_operation(*entry);
          }
      }
      finish(op, 0);
      return;
    }
    if (op.request.kind == operation_kind::SENDMSG)
      op.request.message.msg_iov = op.request.vectors.data();
    const auto result = windows_operations::perform(op.request, op.connecting);
    if (result == -EAGAIN || result == -EWOULDBLOCK || result == -EINPROGRESS
        || result == -EALREADY) {
      if (!resource) {
        finish(op, result);
        return;
      }
      resource->readiness &= ~direction_of(op.request.kind);
      if (const int error = ensure_registration(*resource); error)
        finish(op, error);
      return;
    }
    finish(op, result);
  }

  /**
   *  @brief 有界文件通道接收稳定 token；即使取消，也须等任务实际结束后才释放租约。
   */
  void submit_file(operation_state& op) noexcept {
    op.running_file = true;
    auto self = shared_from_this();
    const auto token = op.token;
    const auto submitted = fs_->try_submit([self, token] {
      operation_state* operation;
      {
        std::lock_guard lock(self->mutex_);
        operation = self->lookup(token);
        if (operation->cancellation) {
          operation->running_file = false;
          self->finish(*operation, -operation->cancellation);
          self->wake();
          return;
        }
      }
      bool connecting = false;
      if (operation->request.kind == operation_kind::CLOSE
          && operation->request.native_kind == native_handle_kind::WINDOWS_HANDLE)
        self->forget_native_handle(operation->request.fd, native_handle_kind::WINDOWS_HANDLE);
      const auto result = windows_operations::perform(operation->request, connecting);
      {
        std::lock_guard lock(self->mutex_);
        operation->running_file = false;
        self->finish(*operation, result);
      }
      self->wake();
    });
    if (!submitted) {
      op.running_file = false;
      finish(op, -submitted.error().value());
    }
  }

  /**  @brief 终态仅发布一次；阻塞任务结束前不得进入终态。 */
  void finish(operation_state& op, std::int64_t result) noexcept {
    if (op.terminal || op.running_file || op.native_inflight)
      return;
    if (op.cancellation && result >= 0
        && ((op.request.kind == operation_kind::ACCEPT
             || op.request.kind == operation_kind::ACCEPT_NOWAIT)
            || op.request.kind == operation_kind::OPEN || op.request.kind == operation_kind::OPEN2
            || op.request.kind == operation_kind::SOCKET)) {
      const native_descriptor fd = static_cast<native_descriptor>(result);
      if (!defer_native_close(fd))
        defer_cleanup([fd,
                       kind = op.request.kind == operation_kind::OPEN
                                  ? native_handle_kind::WINDOWS_HANDLE
                                  : native_handle_kind::WINDOWS_SOCKET] {
          (void)windows_operations::close_native(fd, kind);
        });
    }
    op.terminal = true;
    op.transferred = result > 0 && direction_of(op.request.kind)
                             && op.request.kind != operation_kind::ACCEPT
                             && op.request.kind != operation_kind::ACCEPT_NOWAIT
                             && op.request.kind != operation_kind::CONNECT
                         ? static_cast<std::uint64_t>(result)
                         : 0;
    if (op.request.kind == operation_kind::UNLINKAT && result == 0)
      op.transferred = 1;
    if (op.request.kind == operation_kind::READY && result >= 0 && op.request.resource)
      op.transferred = op.request.resource->readiness_generation;
    op.result = op.cancellation ? -op.cancellation : result;
    const auto& resource = op.request.resource;
    if (resource) {
      if (op.uses_reader && resource->reader == &op)
        resource->reader = nullptr;
      if (op.uses_writer && resource->writer == &op)
        resource->writer = nullptr;
      if (op.observer) {
        auto** link = &resource->observers;
        while (*link && *link != &op)
          link = &(*link)->observer_next;
        if (*link)
          *link = op.observer_next;
      }
      if (op.counts_active)
        --resource->active;
      if (resource->write_shutdown && !resource->writer && !resource->write_reserved
          && resource->fd() >= 0)
        issue_shutdown_write(*resource);
    }
    op.completed_previous = completed_tail_;
    op.next = nullptr;
    op.queued_completion = true;
    if (completed_tail_)
      completed_tail_->next = &op;
    else
      completed_head_ = &op;
    completed_tail_ = &op;
    if (resource && resource->closing && !resource->active)
      close_handle(*resource);
    else if (resource && stopped() && draining_ && !resource->active)
      begin_close(*resource, nullptr);
  }

  /**  @brief 发布 closing 状态后拒绝后续 I/O，并取消、排空已接受的请求。 */
  void begin_close(resource_state& resource, operation_state* waiter) noexcept {
    if (waiter) {
      waiter->request.uncancellable = true;
      waiter->observer_next = resource.close_waiter;
      resource.close_waiter = waiter;
    }
    resource.closing = true;
    for (auto& entry : operations_)
      if (entry->allocated && entry->accepted && !entry->terminal && entry.get() != waiter
          && entry->request.resource.get() == &resource
          && entry->request.kind != operation_kind::CLOSE)
        if (!entry->cancellation)
          entry->cancellation = ECANCELED;
    auto* reader = resource.reader;
    auto* writer = resource.writer;
    if (reader) {
      if (!reader->cancellation)
        reader->cancellation = ECANCELED;
      cancel_operation(*reader);
    }
    if (writer) {
      if (!writer->cancellation)
        writer->cancellation = ECANCELED;
      cancel_operation(*writer);
    }
    auto* observer = resource.observers;
    while (observer) {
      auto* next = observer->observer_next;
      cancel_operation(*observer);
      observer = next;
    }
    if (!resource.active)
      close_handle(resource);
  }

  /**
   *  @brief 方向和操作租约排空后独占接管 fd；文件和启用 linger 的 socket 由清理通道
   * 实际关闭后，才通知等待者完成。
   * @details handle.exchange(-1) 拒绝新 IO，但不代表内核 close 已完成；不能重复
   * close。
   */
  void close_handle(resource_state& resource) noexcept {
    if (resource.close_pending)
      return;
    const native_descriptor fd = resource.handle.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
      if (resource.registered)
        backend_.unregister_resource({static_cast<std::uintptr_t>(fd), resource.native_kind});
      resource.registered = false;
      descriptors_.erase(fd);
      if (resource.owns_handle) {
        if (windows_operations::needs_deferred_close(resource, fd)) {
          auto held = resources_[resource.id].lock();
          if (held) {
            resource.close_pending = true;
            defer_cleanup([held, fd] {
              const int error = windows_operations::close_native(fd, held->native_kind);
              {
                std::lock_guard lock(held->owner->mutex_);
                held->close_pending = false;
                held->owner->finish_close_waiters(*held, error);
              }
            });
            return;
          }
        }
        (void)windows_operations::close_native(fd, resource.native_kind);
      }
    }
    finish_close_waiters(resource, 0);
  }

  /** @brief 摘除关闭等待链，并让各等待操作取得同一关闭结果。 */
  void finish_close_waiters(resource_state& resource, int error) noexcept {
    auto* waiter = std::exchange(resource.close_waiter, nullptr);
    while (waiter) {
      auto* next = waiter->observer_next;
      finish(*waiter, error ? -error : 0);
      waiter = next;
    }
  }

  /**  @brief 在锁内摘除一项待发布的完成；摘链后仍保持已分配状态，在 publish 返回后回收。 */
  void remove_completed(operation_state& op) noexcept {
    if (op.completed_previous)
      op.completed_previous->next = op.next;
    else
      completed_head_ = op.next;
    if (op.next)
      op.next->completed_previous = op.completed_previous;
    else
      completed_tail_ = op.completed_previous;
    op.next = nullptr;
    op.completed_previous = nullptr;
    op.queued_completion = false;
  }

  /**  @brief 在锁内释放结果及资源租约，再允许下一代复用槽位。 */
  void recycle_completed(operation_state& op) noexcept {
    op.allocated = false;
    op.accepted = false;
    op.target = {};
    if (op.slot == UINT32_MAX) {
      op.request = io_request{};
      native_controls_.erase(op.token.value);
      return;
    }
    op.request.resource.reset();
    std::vector<iovec>{}.swap(op.request.vectors);
    std::string{}.swap(op.request.path);
    std::string{}.swap(op.request.path2);
    free_.push_back(op.slot);
  }

  /**
   * @brief 在域锁内独占领取当前 token 的完成结果，不回调、不回收，也不移动稳定请求。
   * @return 空快照表示尚未完成，或已由其他发布者领取，不能重复交付。
   */
  claimed_completion claim_completed_locked(operation_token token) noexcept {
    auto* operation = lookup(token);
    if (!operation || !operation->queued_completion)
      return {};
    remove_completed(*operation);
    return {operation, operation->target, operation->result, operation->transferred};
  }

  /**
   * @brief 在锁外交付已领取的快照，回调返回后重新取得原域锁，回收稳定槽。
   * @details 本地即时完成路径退出 driver、session 和 domain 作用域后才调用；
   * 其他入口保持原有的作用域约定。
   */
  void publish_claimed(claimed_completion delivery) noexcept {
    auto control = std::move(delivery.operation->control_completion);
    if (control)
      control(delivery.result < 0 ? static_cast<int>(-delivery.result) : 0);
    delivery.target.publish(delivery.result, delivery.transferred);
    {
      std::lock_guard lock(mutex_);
      recycle_completed(*delivery.operation);
      if (free_.size() + retired_slots_ == operations_.size())
        quiescent_cv_.notify_all();
    }
  }

  /**
   * @brief 仅交付 submit 当前 token 的即时完成；其余完成留给各自所属的驱动器。
   */
  void publish_one(operation_token token) noexcept {
    claimed_completion delivery;
    {
      std::lock_guard lock(mutex_);
      delivery = claim_completed_locked(token);
    }
    if (delivery.operation)
      publish_claimed(delivery);
  }

  /**
   *  @brief 驱动器每批最多领取 64 项完成，在锁外发布后批量回收。
   * @details 消费者可以重入，也可能在恢复后销毁 awaiter，因此先复制全部输出，
   * 在回调返回后才复用槽位。回调新增的完成仍挂在域链表中，由后续批次处理，
   * 不改变原有预算限制。
   */
  std::size_t publish_completed(std::size_t budget) noexcept {
    struct delivery {
      operation_state* operation; /**< 待交付操作的稳定槽，回调返回后才可回收。 */
      completion_target target;   /**< 通过单个指针引用的完成节点。 */

      std::int64_t result;       /**< 已完成操作的返回值；具体错误解释遵循所属接口约定。 */
      std::uint64_t transferred; /**< 操作已完成的字节数，失败或取消时仍保留有效进度。 */
    };

    std::array<delivery, 64> batch;
    std::size_t published = 0;
    while (published < budget) {
      std::size_t count = 0;
      {
        std::lock_guard lock(mutex_);
        const auto limit = std::min(batch.size(), budget - published);
        while (count < limit && completed_head_) {
          auto* operation = completed_head_;
          remove_completed(*operation);

          batch[count++] = {
              operation, operation->target, operation->result, operation->transferred};
        }
        if (!count) {
          flush_deferred_cleanup();

          break;
        }
      }
      for (std::size_t i = 0; i < count; ++i) {
        const auto entry = batch[i];
        auto control = std::move(entry.operation->control_completion);
        if (control)
          control(entry.result < 0 ? static_cast<int>(-entry.result) : 0);
        entry.target.publish(entry.result, entry.transferred);
      }
      {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < count; ++i)
          recycle_completed(*batch[i].operation);
        flush_deferred_cleanup();
        if (free_.size() + retired_slots_ == operations_.size())
          quiescent_cv_.notify_all();
      }
      published += count;
    }
    return published;
  }

  /**
   * @brief 在域锁内将已保存的清理队列转交专属通道，不在 I/O worker 中执行 close。
   */
  void flush_deferred_cleanup() noexcept {
    while (!deferred_cleanup_.empty()) {
      auto job = deferred_cleanup_.front();
      auto self = shared_from_this();
      auto result = cleanup_->try_submit([self, job] {
        (*job)();
        if (self->publishers_.fetch_sub(1, std::memory_order_acq_rel) == 1)
          self->quiescent_cv_.notify_all();
        self->wake();
      });
      if (!result)
        break;
      deferred_cleanup_.pop_front();
    }
  }

  /** @brief 把更早的操作绝对截止时间并入本域最近截止时间缓存。 */
  void update_deadline(std::chrono::steady_clock::time_point time) noexcept {
    if (!next_deadline_ || time < *next_deadline_)
      next_deadline_ = time;
  }

  /** @brief 扫描已到期的稳定操作，固定超时原因并按当前执行阶段推进安全取消。 */
  void expire_deadlines() noexcept {
    if (!next_deadline_)
      return;

    const auto now = std::chrono::steady_clock::now();
    if (*next_deadline_ > now)
      return;
    next_deadline_.reset();
    for (auto& entry : operations_) {
      auto& op = *entry;
      if (!op.allocated || !op.accepted || op.terminal || op.request.uncancellable
          || !op.request.deadline)
        continue;
      if (*op.request.deadline <= now) {
        if (!op.cancellation)
          op.cancellation = ETIMEDOUT;
        cancel_operation(op);
      } else
        update_deadline(*op.request.deadline);
    }
  }

  backend_box backend_; /**< 域唯一拥有的静态后端包装，负责原生提交、轮询与控制唤醒。 */
  std::deque<operation_token>
      native_pending_; /**< 等待原生提交容量的操作令牌 FIFO，不保存可失效的槽位指针。 */
  std::unordered_map<std::uint64_t, std::unique_ptr<operation_state>>
      native_controls_;             /**< 独立于普通操作池的原生控制状态，覆盖异步关闭等清理责任。 */
  std::uint32_t next_control_{};    /**< 独立控制代际不复用，也不占用业务 token 的高位。 */
  const std::size_t max_resources_; /**< 同一 IO 域允许登记的长期资源数量上限。 */
  mutable std::recursive_mutex
      mutex_; /**< 资源租约释放可能在当前状态转换里重入注销，因此使用可重入控制面锁。 */
  std::mutex driver_mutex_; /**< 串行化后端驱动，防止多个线程同时消费同一完成队列。 */
  std::condition_variable_any
      quiescent_cv_; /**< 关闭方等待业务操作、发布者和清理责任全部排空的条件变量。 */
  std::vector<std::unique_ptr<operation_state>>
      operations_;                  /**< 预分配且地址稳定的操作池，内核解除引用后才允许复用槽位。 */
  std::vector<std::uint32_t> free_; /**< 可复用操作槽或条目的集合，内核解除引用后才允许归还。 */
  std::size_t retired_slots_{};     /**< generation 用尽的槽，既非可用容量也非在途操作。 */
  std::unordered_map<std::uint64_t, std::weak_ptr<resource_state>>
      resources_; /**< 资源键到资源租约弱引用的索引，迟到事件须核对单调资源身份。 */
  std::unordered_map<native_descriptor, std::weak_ptr<resource_state>>
      descriptors_;                   /**< 原生句柄到资源租约弱引用的索引，用于复用既有资源归属。 */
  std::uint64_t next_resource_{};     /**< 下一个单调资源身份，零保留给后端唤醒通道。 */
  operation_state* completed_head_{}; /**< 域内等待锁外发布的完成链表头。 */
  operation_state* completed_tail_{}; /**< 完成链表尾，追加完成时无需分配或遍历。 */
  std::optional<std::chrono::steady_clock::time_point>
      next_deadline_;           /**< 当前操作中最早的绝对截止时间，驱动据此限制内核等待时长。 */
  std::atomic<bool> stopped_{}; /**< 停止状态的跨线程发布标志，后续接纳和查询据此拒绝新工作。 */
  int fatal_error_{};           /**< 首次永久驱动故障；在内核引用排空前禁止结果回收。 */
  bool draining_{};             /**< 域是否处于排空阶段，已接管的完成责任仍须履行。 */
  std::stop_source stop_;       /**< 本操作或任务图的停止源，向已登记等待者广播协作取消。 */
  std::shared_ptr<blocking::service> fs_, dns_,
      cleanup_; /**< 共享关闭清理服务，不能因普通请求容量不足丢弃清理责任。 */
  bool own_fs_{}, own_dns_{},
      own_cleanup_{}; /**< 清理服务是否由本域创建，决定析构时是否负责关闭。 */
  std::shared_ptr<io_placement_group>
      placement_;                         /**< 同一运行时多个 IO 域之间的资源放置服务租约。 */
  std::atomic<std::size_t> publishers_{}; /**< 仍在锁外发布结果的线程数，域静止必须等待它们退出。 */
  std::unordered_map<std::uint64_t, ::faio::move_only_function<void()>>
      shutdown_cleanups_;        /**< 持有回调自身状态的资源清理表，停机时必须执行。 */
  std::uint64_t next_cleanup_{}; /**< 单调分配的停机清理登记编号。 */
  std::deque<std::shared_ptr<::faio::move_only_function<void()>>>
      deferred_cleanup_; /**< 服务暂时饱和时保留的清理任务，后续驱动负责重试。 */
};

}  // namespace faio::io::detail
