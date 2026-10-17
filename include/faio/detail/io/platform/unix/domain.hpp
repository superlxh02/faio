/**
 * @file faio/detail/io/platform/unix/domain.hpp
 * @brief 跨平台异步 IO：资源与操作状态域。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#if !defined(_WIN32)
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
#include "faio/detail/io/platform/unix/request_operations.hpp"
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
 * @brief 统一管理资源身份、稳定操作槽、方向执行权及 IO 完成的 POSIX 域。
 * @details mutex_ 串行化接纳、取消和终态，driver_mutex_ 保证单一事件消费者。
 * 原生后端保留请求到最终内核引用解除；就绪后端先执行非阻塞调用，
 * 暂不可用时才登记等待。消费者始终在域锁外调用，返回后才回收槽位。
 */
class io_domain : public std::enable_shared_from_this<io_domain> {
 public:
  explicit io_domain(engine_config config)
      : backend_(
#if defined(FAIO_TEST_BACKEND_INJECTION)
            config.backend_factory ? config.backend_factory()
            : config.reactor_factory
                ? backend_box{std::make_unique<readiness_adapter>(config.reactor_factory())}
                :
#endif
                make_platform_backend(
#if defined(__linux__)
                    config.requested_backend,
#endif
                    config.native_queue_entries)),
        max_resources_(config.max_resources),
        fs_(config.filesystem_service
                ? config.filesystem_service
                : std::make_shared<execution::blocking_executor>(
                      config.filesystem_threads,
                      config.blocking_queue_limit,
                      1,
                      backend_.native() ? execution::executor_startup::on_demand
                                        : execution::executor_startup::preheated)),
        dns_(config.resolver_service
                 ? config.resolver_service
                 : std::make_shared<execution::blocking_executor>(
                       config.resolver_threads,
                       config.blocking_queue_limit,
                       1,
                       backend_.native() ? execution::executor_startup::on_demand
                                         : execution::executor_startup::preheated)),
        cleanup_(config.cleanup_service
                     ? config.cleanup_service
                     : std::make_shared<execution::blocking_executor>(
                           1,
                           config.blocking_queue_limit,
                           1,
                           backend_.native() ? execution::executor_startup::on_demand
                                             : execution::executor_startup::preheated)),
        own_fs_(!config.filesystem_service),
        own_dns_(!config.resolver_service),
        own_cleanup_(!config.cleanup_service),
        placement_(std::move(config.placement_service)) {
    /// @brief 全部操作槽在初始化阶段预热，普通 read/write 热路径不向通用堆申请内存。
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

  ~io_domain() {
    if (own_fs_)
      fs_->close();
    if (own_dns_)
      dns_->close();
    if (own_cleanup_)
      cleanup_->close();
  }

  io_domain(const io_domain&) = delete;

  /// @brief 读取当前组件是否已进入停止状态。
  bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

  /// @brief 取得所属任务或服务的协作停止令牌。
  std::stop_token stop_token() const noexcept { return stop_.get_token(); }

  /// @brief 取得文件或通用阻塞执行服务引用，具体服务归属由上下文决定。
  execution::blocking_executor_ref blocking() const noexcept {
    return execution::blocking_executor_ref{fs_};
  }

  /// @brief 取得专门执行不可丢弃关闭工作的清理服务引用。
  execution::blocking_executor_ref cleanup() const noexcept {
    return execution::blocking_executor_ref{cleanup_};
  }

  /// @brief 取得隔离地址解析阻塞调用的执行服务引用。
  execution::blocking_executor_ref resolver() const noexcept {
    return execution::blocking_executor_ref{dns_};
  }

  /// @brief 返回实际已初始化后端提供的能力快照。
  io_capabilities capabilities() const noexcept {
    io_capabilities c;
    c.backend = backend_.name();
    c.native_filesystem = backend_.native() && supports_native(operation_kind::read)
                          && supports_native(operation_kind::write)
                          && supports_native(operation_kind::open)
                          && supports_native(operation_kind::fsync);
    c.zero_copy = supports_native(operation_kind::send_zc);
    c.native_accept_nowait = supports_native(operation_kind::accept_nowait);
    return c;
  }

  /// @brief 查询初始化时固定的原生能力，避免普通请求热路径重复探测平台。
  bool supports_native(operation_kind kind) const noexcept {
    return backend_.native() && backend_.supports(static_cast<std::uint32_t>(kind));
  }

  /// @brief 读取后端累计提交、完成与控制事件统计。
  backend_statistics statistics() const noexcept { return backend_.statistics(); }

  /// @brief 发送独立控制通知，打断底层等待以处理已经发布的工作。
  void wake() const noexcept { backend_.wake(); }

  /// @brief 从运行时放置服务选择 IO 域，未配置放置服务时沿用当前域。
  io_context balanced_context() noexcept {
    if (placement_)
      return io_context{placement_->select()};
    return stopped() ? io_context{} : io_context{shared_from_this()};
  }

  /** @brief 只读取本域已经登记的 fd 身份；查找不会执行系统调用或借用注册。
   * @details 弱引用提升与 handle 核对在域锁内完成，返回的拥有型租约固定原资源。
   */
  resource_ptr find_registered_resource(int fd) noexcept {
    /// @brief 与注册、关闭及描述符表更新使用相同短锁。
    std::lock_guard lock(mutex_);
    /// @brief 未登记时不得创建另一份资源身份。
    const auto found = descriptors_.find(fd);
    if (found == descriptors_.end())
      return {};
    /// @brief 操作准备前先保留稳定资源和 owner 的租约。
    auto resource = found->second.lock();
    return resource && resource->fd() == fd ? std::move(resource) : resource_ptr{};
  }

  /** @brief raw fd 冷路径复用已有归属；带 resource 的正常 IO 不调用此接口。
   * @param include_runtime 是否允许查询本 runtime 的其他 shard；显式 context
   * 只查本域。
   * @details 本域锁释放后才读取弱 shard 表，任何时候都不同时持有两种控制面锁。
   */
  resource_ptr find_raw_resource(int fd, bool include_runtime) noexcept {
    /// @brief 已登记本域身份优先，不迁移资源或改变 fd 所有权。
    if (auto resource = find_registered_resource(fd))
      return resource;
    return include_runtime && placement_ ? placement_->find_resource(fd, this) : resource_ptr{};
  }

  /** @brief 注册长期资源；不接管失败的 fd，所有权仍归调用方。 */
  resource_ptr adopt(int fd, bool owns = true, bool regular = false) {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
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
    /// @brief key 永不复用；零 key 预留为唤醒通道。
    r->id = ++next_resource_;
    if (!r->id)
      throw std::overflow_error("IO resource token exhausted");
    r->handle.store(fd, std::memory_order_release);
    r->regular_file = regular;
    /// @brief fd family 与绑定归属一起固定，native 扩展不能盲用于 Unix/pipe。
    r->socket_family = unix_operations::query_socket_family(
        fd);
    resources_.emplace(r->id, r);
    descriptors_[fd] = r;
    /// @brief 只有所有注册成功后才接管 fd。
    r->owns_handle = owns;
    return r;
  }

  /** @brief 借用 fd 的兼容注册；长期高性能路径应传 resource_ptr。 */
  resource_ptr borrow(int fd) {
    if (auto it = descriptors_.find(fd); it != descriptors_.end())
      if (auto r = it->second.lock())
        return r;
    const bool regular = unix_operations::prepare_borrowed_descriptor(fd);
    return adopt(fd, false, regular);
  }

  /** @brief 原生导入对象首次挂起时绑定归属，保留原共享控制块。 */
  void bind(const resource_ptr& resource) {
    /// @brief 首次归属在资源局部串行，跨 domain 安全。
    std::lock_guard binding_lock(
        resource->binding_mutex);
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    if (resource->owner) {
      /// @brief 资源已有固定归属时拒绝跨域提交，协程迁移不能隐式迁移资源。
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
    resource->socket_family = unix_operations::query_socket_family(resource->fd());
    resource->owner = shared_from_this();
  }

  /// @brief 在资源状态锁内取得指定方向的组合操作预留。
  expected<void> reserve(resource_state& resource, Interest interest, void* owner) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
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

  /// @brief 只释放由同一稳定身份持有的方向预留，并推进等待关闭的资源。
  void unreserve(resource_state& resource, Interest interest, void* owner) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    const auto bits = static_cast<unsigned>(interest);
    if (bits & 1 && resource.read_reserved == owner)
      resource.read_reserved = nullptr;
    if (bits & 2 && resource.write_reserved == owner) {
      resource.write_reserved = nullptr;
      /// @brief 组合写操作保留整段写方向；最后租约退出才允许此前请求的半关闭。
      if (resource.write_shutdown && !resource.writer && resource.fd() >= 0)
        issue_shutdown_write(resource);
    }
  }

  /** @brief 最后控制块退出；注销和 fd 接管都在同一个串行状态协议中完成。 */
  void release_resource(resource_state& resource) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    /// @brief 唯一接管原生关闭责任。
    const int fd =
        resource.handle.exchange(-1, std::memory_order_acq_rel);
    if (resource.registered && fd >= 0)
      backend_.detach(fd);
    resources_.erase(resource.id);
    if (fd >= 0)
      descriptors_.erase(fd);
    if (resource.owns_handle && fd >= 0) {
      /// @brief 原生析构直接接管CLOSE SQE，不启动cleanup线程。
      if (defer_native_close(fd))
        return;
      /// @brief POSIX close 不能因 EINTR 重试一个可能已复用的 fd。
      if (unix_operations::needs_deferred_close(resource, fd))
        defer_cleanup([fd] { (void)unix_operations::close_descriptor(fd); });
      else
        (void)unix_operations::close_descriptor(
            fd);
    }
  }

  /** @brief 仅在无在途请求时导出所有权，所有 split 包装同时观察 detached。 */
  expected<int> detach(resource_state& resource) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    if (resource.active || resource.close_waiter || resource.read_reserved
        || resource.write_reserved)
      return std::unexpected{make_error(EBUSY)};
    /// @brief 唯一接管原生关闭责任。
    const int fd =
        resource.handle.exchange(-1, std::memory_order_acq_rel);
    if (fd < 0)
      return std::unexpected{make_error(EBADF)};
    /// @brief 已有持久就绪登记时复用该登记，避免逐次修改内核兴趣集。
    if (resource.registered)
      backend_.detach(fd);
    resource.registered = false;
    /// @brief 先发布 closing，后续 syscall 无法跨关闭边界取得执行权。
    resource.closing = true;
    resource.owns_handle = false;
    resources_.erase(resource.id);
    descriptors_.erase(fd);
    return fd;
  }

  /** @brief 非阻塞瞬时 IO 在短锁内执行，完成后不留下内核/队列/消费者引用。
   * @details 只有 scalar 流/报文 IO 可进入；资源租约和方向 gate 校验与正常
   * submit 相同。 EAGAIN 回退 stable
   * prepare/submit；已注册且没有新事件时复用本次 EAGAIN。 新注册及新 readiness
   * generation 仍执行真实 attempt，保留 ET 不丢事件协议。
   *          普通文件永不进入此路径；取消、close 与 syscall 在同一锁内线性化。
   */
  std::optional<immediate_result> try_immediate(io_request& request,
                                                bool already_stopped) noexcept {
    /// @brief 原完整请求入口保留。
    return try_immediate_impl(request, already_stopped);
  }

  /** @brief 紧凑 RECV/SEND 与完整请求共用资源锁、停止、deadline 和方向校验。 */
  std::optional<immediate_result> try_immediate(scalar_io_request& request,
                                                bool already_stopped) noexcept {
    return try_immediate_impl(request, already_stopped);
  }

 private:
  /// @brief 两个明确的请求策略共用同一状态机，避免紧凑路径遗漏取消/关闭/方向规则。
  template <class Request>
  std::optional<immediate_result> try_immediate_impl(Request& request,
                                                     bool already_stopped) noexcept {
    /// @brief 原生 Proactor 操作全部经 SQ/CQ，不能用同步 syscall 冒充 native 完成。
    if (backend_.native())
      return {};

    if constexpr (std::is_same_v<Request, scalar_io_request>) {
      /// @brief 紧凑策略没有其他 opcode 的输入，不能将其解释为 read/write。
      if (request.kind != operation_kind::recv && request.kind != operation_kind::send)
        return {};

    } else if (request.kind != operation_kind::recv && request.kind != operation_kind::send
               && request.kind != operation_kind::read && request.kind != operation_kind::write) {
      return {};
    }
    /// @brief awaiter 的请求租约已覆盖整个瞬时调用，无需重复计数。
    const auto& resource =
        request.resource;
    /// @brief raw fd 尚未借用以及阻塞文件沿用稳定槽协议。
    if (!resource || resource->regular_file)
      return {};
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    /// @brief 任务停止或域已关闭时，在真实系统调用前固定取消结果。
    if (already_stopped || stopped())
      /// @brief 取消尚未执行的请求，返回零进度且不留下任何内核或完成队列引用。
      return immediate_result{-ECANCELED, 0};
    /// @brief 请求构造阶段发现的参数错误优先返回，不取得资源方向或发起系统调用。
    if (request.validation_error)
      /// @brief 返回已发现的参数错误，调用方缓冲区未被本次请求使用。
      return immediate_result{-request.validation_error, 0};
    /// @brief 资源已有固定归属时拒绝跨域提交，协程迁移不能隐式迁移资源。
    if (resource->owner.get() != this)
      /// @brief 报告资源域不匹配，保留原有归属与方向状态。
      return immediate_result{-EXDEV, 0};
    /// @brief 已进入关闭或原生句柄失效时拒绝新业务，避免命中数值复用后的句柄。
    if (resource->closing || resource->fd() < 0)
      /// @brief 报告资源不可再使用，禁止向无效或已关闭句柄发起 IO。
      return immediate_result{-EBADF, 0};
    /// @brief 提交时已到期则直接建立超时结果，不再产生 IO 副作用。
    if (request.deadline && *request.deadline <= std::chrono::steady_clock::now())
      /// @brief 请求在提交前已经超时，不产生额外 IO 进度。
      return immediate_result{-ETIMEDOUT, 0};
    /// @brief 按操作种类计算读写执行权，观察请求与数据传输遵循不同 gate 规则。
    const auto direction = direction_of(request.kind);
    /// @brief 非法身份不执行 IO 或占用方向。
    if (request.establish_reservation
        && (!request.reservation || (direction != 1 && direction != 2)))
      return immediate_result{-EINVAL, 0};
    if ((direction & 1
         && (resource->reader
             || (resource->read_reserved && resource->read_reserved != request.reservation)))
        || (direction & 2
            && (resource->writer
                || (resource->write_reserved && resource->write_reserved != request.reservation))))
      /// @brief 方向被其他操作或组合预留占用时明确拒绝，不能并发消费同一方向数据。
      return immediate_result{-EBUSY, 0};
    /// @brief 在同一个资源串行边界取得组合方向预留，后续短读写可沿用同一身份。
    if (request.establish_reservation) {
      /// @brief 只读组合取得读方向预留；其余合法单方向组合取得写方向预留。
      if (direction == 1)
        /// @brief 将读方向独占身份固定为当前组合帧，阻止其他读取插入组合过程。
        resource->read_reserved = request.reservation;
      else
        /// @brief 将写方向独占身份固定为当前组合帧，跨短写与再次挂起保持写入顺序。
        resource->write_reserved = request.reservation;
      /// @brief 与执行权校验/真实 syscall 同一线性化点；EAGAIN 后 prepare
      /// 失败也由外层租约释放。
      request.establish_reservation = false;
    }
    /// @brief syscall 期间 close 无法取得此短锁，fd 不会被复用。
    request.fd = resource->fd();
    const auto result = [&] {
      if constexpr (std::is_same_v<Request, scalar_io_request>) {
        /// @brief 仅消费精确定义的 scalar 输入。
        return unix_operations::perform_socket_scalar(request);
      } else {
        /// @brief 为本次系统调用建立连接握手阶段状态，普通非连接请求不会使用该状态。
        bool connecting = false;
        /// @brief 通用 read/write 等协议保持原入口。
        return unix_operations::perform(request,
                                        connecting);
      }
    }();
    if (result == -EAGAIN || result == -EWOULDBLOCK) {
      /// @brief 真实系统调用返回暂不可用后才清除对应方向提示，保留另一方向的状态。
      resource->readiness &= ~direction;
      /// @brief 记录瞬时路径已经执行过一次真实 IO，后续稳定提交可避免无新事件时重复调用。
      request.observed_would_block = true;
      /// @brief 保存本次 EAGAIN 对应的就绪代数，新事件到来会改变代数并强制再次尝试。
      request.observed_readiness_generation = resource->readiness_generation;
      /// @brief 释放瞬时路径锁后走正常 prepare+submit；不存在悬挂的 buffer 引用。
      return {};

    }
    /// @brief 瞬时调用已安全结束，直接返回实际结果及非负字节进度，无需创建稳定异步槽。
    return immediate_result{result, result > 0 ? static_cast<std::uint64_t>(result) : 0};
  }

 public:
  /** @brief 为桥接层取得代际槽；准备阶段绝不执行 IO。
   * @param request 拥有路径、地址及 iovec
   * 描述符的请求；数据缓冲区租约由调用者保持。
   * @param target 消费者的非抛异常完成入口，内核永远不会保存该地址。
   * @param error 槽容量不足时写入 EAGAIN；此时请求尚未接受。
   * @return 本次槽位的唯一代际 token；零值表示准备失败。
   * @details Prepared 槽可接受 sticky cancel，submit 才取得资源方向并执行
   * syscall。
   */
  operation_token prepare(io_request&& request, completion_target target, int& error) noexcept {
    /// @brief 独立 prepare 的公开协议与 Prepared 取消窗口保持不变。
    std::lock_guard lock(mutex_);
    return prepare_locked(std::move(request), target, error);
  }

  /** @brief 左值请求兼容入口：只复制一次拥有参数，再进入同一稳定槽准备协议。
   * @details 常规 awaiter
   * 使用右值重载而不建立中间请求；此入口保留原左值的所有权。 路径/iovec
   * 分配失败和原按值调用一样发生在准备以前，向调用方传播异常。
   */
  operation_token prepare(const io_request& request, completion_target target, int& error) {
    /// @brief 描述符与路径拥有型复制；不提前注册资源或提交 SQE。
    io_request owned = request;
    return prepare(std::move(owned), target, error);
  }

  /** @brief 接受槽位并推进一次非阻塞 attempt；终态由统一完成链交付。
   * @param token prepare 返回的 token；旧代际及重复提交均无副作用。
   * @details 所有资源 gate 在同一个短锁内取得；文件请求交给隔离执行器。
   *          同步完成也经发布协议，协程桥根据 gate 返回“不挂起”。
   */
  void submit(operation_token token) noexcept {
    /// @brief 先假设无需远程通知，接纳路径根据实际归属与待处理责任设置该标志。
    bool wake_needed = false;
    /// @brief 只有满足本地低负载条件时才允许一次原生完成机会，默认沿常规驱动路径。
    bool native_completion_opportunity = false;
    {
      /// @brief 独立 submit 仍只接受一次，重复/旧代际调用无副作用。
      std::lock_guard lock(mutex_);
      if (!submit_locked(token, wake_needed, native_completion_opportunity))
        return;
    }
    /// @brief 域锁外交付与通知。
    finish_submission(token, wake_needed,
                      native_completion_opportunity);
  }

  /** @brief 协程桥在一个域锁内准备并接受 stable request，省去中间一次互斥往返。
   * @param stop 只在本调用内观察停止状态；不会保存 token 的借用地址或注册回调。
   * @param cancellable false 保持原桥不订阅任务停止的策略，已接管 CLOSE 仍看
   * uncancellable。
   * @details 槽选取/代际/方向 gate/原生 SQE 全部复用独立 prepare/submit
   * 的同一状态机。 Prepared 后、accepted 前检查 sticky cancel；发布/driver/wake
   * 全部在域锁外。 target 可在返回以前立即完成，调用方须像原桥一样先建立 arming
   * gate 与拥有租约。
   */
  operation_token prepare_submit(io_request&& request,
                                 completion_target target,
                                 int& error,
                                 const std::stop_token& stop = {},
                                 bool cancellable = true) noexcept {
    /// @brief 暂存新取得的完整代际，锁外发布仍按此身份领取结果。
    operation_token token;
    /// @brief 先假设无需远程通知，接纳路径根据实际归属与待处理责任设置该标志。
    bool wake_needed = false;
    /// @brief 只有满足本地低负载条件时才允许一次原生完成机会，默认沿常规驱动路径。
    bool native_completion_opportunity = false;
    {
      /// @brief prepare → sticky cancel → accept 共用唯一域锁。
      std::lock_guard lock(mutex_);
      token = prepare_locked(std::move(request), target, error);
      /// @brief 尚未取得槽，不提交 SQE、不保存 consumer，也不发布虚假完成。
      if (!token.value)
        return {};

      /// @brief 在接纳之前观察任务停止并记录粘性原因，避免取消窗口遗漏已请求停止。
      if (cancellable && stop.stop_requested()) {
        /// @brief 新 Prepared 完整代际仍固定在本次域锁内。
        auto* operation = lookup(token);
        /// @brief 首次取消原因在 accepted 以前 sticky 保存。
        if (operation && !operation->request.uncancellable && !operation->cancellation)
          operation->cancellation = ECANCELED;
      }
      /// @brief 本调用刚取得的 Prepared 槽不能被另一个接受者抢走。
      if (!submit_locked(token, wake_needed, native_completion_opportunity))
        std::terminate();
    }
    /// @brief 先释放域锁，保持原锁序。
    finish_submission(token,
                      wake_needed,
                      native_completion_opportunity);
    /// @brief gate 为 arming 的消费者只观察结果，不能在此返回以前销毁调用帧。
    return token;

  }

  /** @brief 取消只按代际 token 查槽，Prepared 阶段的取消同样 sticky 保留。 */
  void request_cancel(operation_token token, cancel_reason reason = cancel_reason::user) noexcept {
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      if (auto* op = lookup(token); op && !op->terminal && !op->request.uncancellable) {
        if (!op->cancellation)
          op->cancellation = reason == cancel_reason::deadline ? ETIMEDOUT : ECANCELED;
        if (op->accepted)
          cancel_operation(*op);
      }
    }
    wake();
  }

  /** @brief 最后包装对象析构请求 close；不等待，不依赖当前线程 TLS。 */
  void request_close(const resource_ptr& resource) noexcept {
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      begin_close(*resource, nullptr);
    }
    wake();
  }

  /** @brief 请求写半关闭；单次写和组合写方向租约全部排空后才发 SHUT_WR。 */
  void request_shutdown_write(resource_state& resource) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    resource.write_shutdown = true;
    if (!resource.writer && !resource.write_reserved && resource.fd() >= 0)
      issue_shutdown_write(resource);
  }

  /** @brief 接管孤立fd的原生关闭责任；false表示后端不具备原生CLOSE。
   * @details true之后调用者不得再close；稳定记录/SQ背压均由domain持有到CQE。
   */
  bool defer_native_close(int fd, ::faio::move_only_function<void(int)> completion = {}) noexcept {
    if (fd < 0 || !supports_native(operation_kind::close))
      return false;
    io_request request;
    request.kind = operation_kind::close;
    request.fd = fd;
    request.bypass_resource_registration = true;
    request.uncancellable = true;
    request.internal_control = true;
    /// @brief 保存准备阶段错误码，准备失败表示域尚未接管异步完成责任。
    int error{};
    const auto token = prepare(std::move(request), {}, error);
    /// @brief 原生close不允许丢失已接管的唯一责任。
    if (!token.value)
      std::terminate();
    if (completion) {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      auto* operation = lookup(token);
      operation->control_completion = std::move(completion);
      operation->target = {
          operation, +[](void* consumer, std::int64_t result, std::uint64_t) noexcept {
            auto& op = *static_cast<operation_state*>(consumer);
            auto callback = std::move(op.control_completion);
            /// @brief errno为正，成功为零。
            callback(result < 0 ? static_cast<int>(-result) : 0);
          }};
    }
    submit(token);
    return true;
  }

  /** @brief 公平有界驱动；批量内核事件、取消、deadline 和完成共用一套状态协议。
   */
  drive_result drive(drive_budget budget = {}, std::optional<int> wait = 0) noexcept {
    /// @brief 同线程回调重入不再尝试已持有的 driver mutex。
    if (current_driver_domain)
      return {0, false, false, EBUSY};
    /// @brief 逻辑 session 禁止同时驱动，返回错误而非并发读取同一内核完成队列。
    std::unique_lock driver_lock(driver_mutex_, std::try_to_lock);
    /// @brief 其他线程已经拥有后端消费会话时，不再并发读取同一完成队列。
    if (!driver_lock.owns_lock())
      return {0, false, false, EBUSY};
    /// @brief 锁成功后才发布 session，退出前自动还原。
    driver_session_guard session{this};
    /// @brief 后端完整写入 count 个输出槽；不清零或读取未使用的整批空间。
    std::array<backend_event, 256>
        events;
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      /// @brief 在已有容量内重试积压原生请求，仍满载的后续责任留在队列中。
      pump_native_pending();
      /// @brief 先处理已到期请求，再计算本次内核等待上限。
      expire_deadlines();
      /// @brief 已有可发布结果时必须把本次内核等待改为非阻塞，先交付用户完成。
      if (completed_head_)
        /// @brief 已有本地进展或控制通知时禁止继续阻塞内核等待。
        wait = 0;
      /// @brief 若有操作截止时间，则用其剩余时长进一步收紧本轮内核等待上限。
      if (next_deadline_) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                                   *next_deadline_ - std::chrono::steady_clock::now())
                                   .count();
        const int ms = static_cast<int>(std::clamp<std::int64_t>(remaining, 0, INT_MAX));
        /// @brief 原等待为无限或晚于截止时间时，改用更早的截止预算。
        if (!wait || ms < *wait)
          /// @brief 保存已钳为非负、且不会溢出原生等待整数范围的毫秒预算。
          wait = ms;
      }
    }
    /// @brief 保存独立提交阶段的永久失败，提交后统一进入域故障处理。
    int flush_error{};
    /// @brief poll 自己完成提交的后端只推进一次 SQ；其他后端保留独立 flush 合同。
    /// 能力在后端类型创建时固定，不能仅按 native() 猜测第三方后端的 poll 行为。
    if (!backend_.poll_flushes_submissions()) {
      /// @brief 未自行提交的后端仍沿用 domain → backend 锁序。
      std::lock_guard lock(mutex_);
      if (!fatal_error_)
        flush_error = backend_.flush().error;
    }
    /// @brief 提交失败必须转入失败排空，使已接受操作仍取得明确终态。
    if (flush_error)
      /// @brief 固定提交故障并启动安全失败处理，不将它误当作普通无事件轮询。
      enter_failure(flush_error);
    const int count = backend_.poll(
        std::span(events).first(std::clamp<std::size_t>(budget.max_events, 1, events.size())),
        wait);
    /// @brief 后端返回负错误表示本轮驱动失败，仍须处理已接纳请求的退出责任。
    if (count < 0) {
      /// @brief 把后端负返回转换为正错误码，记录为域永久故障。
      enter_failure(-count);
      /// @brief 永久 reactor 错误仍交付每个 accepted 操作，不能遗留永不恢复的帧。
      std::lock_guard lock(mutex_);
      for (auto& entry : operations_)
        if (entry->allocated && entry->accepted && !entry->terminal && !entry->running_file
            && !entry->native_inflight && !entry->request.uncancellable)
          finish(*entry, count);
    } else {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      /// @brief 只处理后端实际写入的有效事件，未使用的输出槽不读取也不清零。
      for (int i = 0; i < count; ++i) {
        /// @brief 先复制本条统一事件，后续状态修改不依赖可被复用的原生队列内存。
        const auto event = events[static_cast<std::size_t>(i)];
        /// @brief 控制 wake 不匹配普通资源。
        if (!event.key)
          continue;
        /// @brief 原生业务、取消确认与负载释放按操作代际处理，不能当作 fd 就绪事件。
        if (event.kind != backend_event_kind::readiness) {
          /// @brief 核对操作代际并推进真实原生完成协议，迟到事件不能命中下一代槽。
          complete_native(event);
          continue;
        }
        /// @brief key 是单调资源代际，fd 数值复用不会混淆。
        auto it = resources_.find(event.key);
        /// @brief fd 重用或 detach 后的迟到事件。
        if (it == resources_.end())
          continue;
        /// @brief 提升弱引用以固定当前事件处理期间的资源生命期。
        auto resource = it->second.lock();
        /// @brief 资源已销毁或正在关闭时不再基于就绪提示启动新的数据操作。
        if (!resource || resource->closing)
          continue;
        /// @brief readiness guard 只允许清除自己观察到的这一代。
        ++resource->readiness_generation;

        /// @brief 粘性提示仅用于唤醒，最终字节数/EOF 由 syscall 决定。
        resource->readiness |= event.flags;
        /// @brief 只让读方向持有者响应读就绪或错误，写半关闭不构造虚假读数据。
        if (resource->reader && event.flags & (readable_bit | error_bit))
          /// @brief 让当前读操作执行一次真实非阻塞尝试，短读与 EOF 以系统调用结果为准。
          attempt(*resource->reader);
        /// @brief 只让写方向持有者响应写就绪或错误，读方向关闭状态不直接替代写结果。
        if (resource->writer && event.flags & (writable_bit | error_bit))
          /// @brief 推进当前写操作，暂不可用时继续保持稳定等待节点。
          attempt(*resource->writer);
        /// @brief closed 是附加状态；后端已把 EOF/HUP 映射为真正受影响的方向位。
        auto* observer = resource->observers;
        /// @brief 逐个检查该资源上的就绪观察者，观察者不消耗读写数据。
        while (observer) {
          /// @brief attempt 可能终结并摘除当前 observer。
          auto* next = observer->observer_next;
          /// @brief 按观察掩码决定是否完成当前观察者，完成可能把该节点摘链。
          attempt(*observer);
          /// @brief 使用提前保存的后继继续遍历，不能依赖可能已摘除节点的链接。
          observer = next;
        }
      }
      /// @brief 先处理已到期请求，再计算本次内核等待上限。
      expire_deadlines();
    }
    /// @brief 以至少一条的完成预算进行锁外交付，单轮不能无限处理用户新增完成。
    const auto published = publish_completed(std::max<std::size_t>(budget.max_completions, 1));
    bool more;
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      /// @brief 在已有容量内重试积压原生请求，仍满载的后续责任留在队列中。
      pump_native_pending();
      /// @brief 仍有完成或待提交工作时向宿主报告后续驱动责任。
      more = completed_head_ != nullptr || !native_pending_.empty();
      /// @brief 最后 ACK 无业务完成，也要唤醒控制面 drain。
      if (free_.size() + retired_slots_ == operations_.size() && native_controls_.empty()
          && publishers_.load(std::memory_order_acquire) == 0 && backend_.quiescent())
        quiescent_cv_.notify_all();
    }
    /// @brief 返回完成数量、进展、剩余工作与永久错误，宿主据此选择继续调度或等待。
    return {published, published != 0 || count > 0, more, fatal_error_};
  }

  /// @brief 计算到最近截止时间的非负毫秒数，作为底层等待上限。
  std::optional<int> next_deadline() noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    if (!next_deadline_)
      return {};
    const auto duration = std::chrono::ceil<std::chrono::milliseconds>(
                              *next_deadline_ - std::chrono::steady_clock::now())
                              .count();
    return static_cast<int>(std::clamp<std::int64_t>(duration, 0, INT_MAX));
  }

  /// @brief 检查当前组件是否已经没有尚未归还的执行、内核引用与完成责任。
  bool quiescent() const noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    return free_.size() + retired_slots_ == operations_.size()
           && publishers_.load(std::memory_order_acquire) == 0 && native_controls_.empty()
           && backend_.quiescent();
  }

  /** @brief 控制线程等待驱动器推进排空；不占 CPU 忙轮询，也不抢 poll session。
   */
  void wait_quiescent() noexcept {
    std::unique_lock lock(mutex_);
    quiescent_cv_.wait(lock, [&] {
      return free_.size() + retired_slots_ == operations_.size()
             && publishers_.load(std::memory_order_acquire) == 0 && native_controls_.empty()
             && backend_.quiescent();
    });
  }

  /** @brief 停机先拒绝提交，随后取消、关闭、排空；清理通道保持可用。 */
  void begin_shutdown(shutdown_policy policy = shutdown_policy::cancel_all) noexcept {
    if (stopped_.exchange(true, std::memory_order_acq_rel))
      return;
    /// @brief callback 不持有 domain 状态锁，避免用户停止回调重入死锁。
    if (policy == shutdown_policy::cancel_all)
      stop_.request_stop();

    std::unordered_map<std::uint64_t, ::faio::move_only_function<void()>> cleanups;
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      backend_.begin_shutdown();
      /// @brief drain 保留已接受请求，cancel_all 严格取消。
      draining_ = policy == shutdown_policy::drain;
      /// @brief 回调移出锁外执行，可安全重新进入资源关闭接口。
      cleanups.swap(shutdown_cleanups_);
      if (policy == shutdown_policy::cancel_all) {
        for (auto& entry : operations_)
          if (entry->allocated && !entry->terminal && !entry->request.uncancellable) {
            if (!entry->cancellation)
              entry->cancellation = ECANCELED;
            cancel_operation(*entry);
          }
      }
      /// @brief 保留 weak 表的迭代稳定性，close 在这里不擦除资源表。
      for (auto it = resources_.begin(); it != resources_.end();) {
        auto weak = (it++)->second;
        if (auto resource = weak.lock();
            resource && (policy == shutdown_policy::cancel_all || !resource->active))
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

  /// @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。
  void shutdown() noexcept {
    begin_shutdown();
    /// @brief blocking syscall 不能强行终止，必须等真正返回后才释放借用 buffer。
    while (!quiescent())
      (void)drive({}, 10);
    if (own_fs_)
      fs_->close();
    if (own_dns_)
      dns_->close();
    if (own_cleanup_)
      cleanup_->close();
  }

  /// @brief 登记拥有型停机清理回调，使服务关闭时仍能归还长期资源。
  std::uint64_t register_shutdown_cleanup(::faio::move_only_function<void()> callback) {
    std::uint64_t token{};
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      if (!stopped()) {
        token = ++next_cleanup_;
        if (!token)
          throw std::overflow_error("shutdown cleanup token exhausted");
        shutdown_cleanups_.emplace(token, std::move(callback));
      }
    }
    /// @brief 停机后的注册立即推进清理，不能遗漏刚完成的 open。
    if (!token)
      callback();
    return token;
  }

  /// @brief 撤销已经由正常关闭路径接管的停机清理责任。
  void unregister_shutdown_cleanup(std::uint64_t token) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    /// @brief 正常关闭及时回收注册项，反复 open 不会累积弱回调。
    shutdown_cleanups_.erase(token);
  }

  /** @brief 仅保存停机排空责任；等待外层lease退出时不创建线程/job/request。
   * @details File/ReadDir的唯一关闭owner取得票据，最终CLOSE/closedir后释放。
   */
  void retain_cleanup_wait() noexcept {
    /// @brief 容量计数不得回绕为“已经排空”。
    if (publishers_.fetch_add(1, std::memory_order_acq_rel) == SIZE_MAX)
      std::terminate();
  }

  /// @brief 归还配对的清理等待责任，并在最后责任解除时通知排空方。
  void release_cleanup_wait() noexcept {
    const auto previous = publishers_.fetch_sub(1, std::memory_order_acq_rel);
    /// @brief 每张责任票据只能释放一次。
    if (!previous)
      std::terminate();
    if (previous == 1)
      quiescent_cv_.notify_all();
    /// @brief 真实关闭结束后驱动器可重新判定停机排空。
    wake();
  }

  /** @brief 清理任务由专属 lane 执行；满载时保存在清理链而不是阻塞 worker。 */
  void defer_cleanup(::faio::move_only_function<void()> function) noexcept {
    /// @brief 完成责任用共享拥有型包装，try_submit 失败后仍能保留并重试任务。
    try {
      auto job = std::make_shared<::faio::move_only_function<void()>>(std::move(function));
      /// @brief 用共享域租约覆盖服务线程执行与最后完成通知，避免借用已销毁的 worker。
      auto self = shared_from_this();
      publishers_.fetch_add(1, std::memory_order_release);
      auto work = [self, job] {
        (*job)();
        if (self->publishers_.fetch_sub(1, std::memory_order_acq_rel) == 1)
          self->quiescent_cv_.notify_all();
        /// @brief 唤醒所属域驱动器，使后台结果进入原有锁外发布与资源排空流程。
        self->wake();
      };
      if (auto result = cleanup_->try_submit(std::move(work)); !result) {
        /// @brief 队列饱和仍保留任务；借用 buffer 清理必须由 shutdown 等待。
        std::lock_guard lock(mutex_);
        deferred_cleanup_.push_back(std::move(job));
        wake();
      }
    } catch (...) {
      std::terminate();
    /// @brief 无法保存关闭责任时不能静默泄漏或同步阻塞 IO worker。
    }
  }

  /// @brief 读取资源就绪位与观察代际，供就绪守卫安全消费提示。
  std::pair<Ready, std::uint64_t> snapshot_readiness(
      const resource_state& resource) const noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    return {Ready{resource.readiness, resource.readiness_generation},
            resource.readiness_generation};
  }

  /// @brief 仅清除当前观察代际的指定就绪方向，不能抹掉后来收到的新事件。
  void clear_readiness(resource_state& resource,
                       Interest interest,
                       std::uint64_t generation) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    if (resource.readiness_generation == generation)
      resource.readiness &= ~static_cast<std::uint32_t>(interest);
  }

  /// @brief 读取资源就绪位与观察代际，供就绪守卫安全消费提示。
  std::pair<Ready, std::uint64_t> snapshot_readiness(const resource_ptr& resource) const noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    return {
        Ready{resource ? resource->readiness : 0, resource ? resource->readiness_generation : 0},
        resource ? resource->readiness_generation : 0};
  }

  /** @brief 只清除同一代际的观察结果，保留 guard 之后到达的新事件。 */
  void clear_readiness(const resource_ptr& resource,
                       Interest interest,
                       std::uint64_t generation) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    if (resource && resource->readiness_generation == generation)
      resource->readiness &= ~static_cast<std::uint32_t>(interest);
  }

  /** @brief try_* 与异步方向执行权共用同一 gate，EAGAIN 清除相应缓存。 */
  template <class F>
  auto with_resource(const resource_ptr& resource,
                     Interest interest,
                     F&& function,
                     void* reservation = nullptr) -> std::invoke_result_t<F> {
    using result_type = std::invoke_result_t<F>;
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
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
    if (!result && (result.error().value() == EAGAIN || result.error().value() == EWOULDBLOCK))
      resource->readiness &= ~bits;
    return result;
  }

 private:
  /** @brief 已从完成链独占领取的本 token 交付快照；稳定槽仍 allocated 到
   * callback 返回。
   * @details 只复制非拥有目标与标量结果，不移动
   * request/resource，不提前开放下一代槽。
   */
  struct claimed_completion {
    operation_state* operation{};  ///< 空指针表示尚未领取；没有新完成或回收责任。
    completion_target target{};  ///< callback 前复制；发布以后不再访问 consumer 地址。
    std::int64_t result{};  ///< 原最终 CQE/统一 finish 产生的业务结果。
    std::uint64_t transferred{};  ///< 统一状态机保存的实际进度。
  };

  /** @brief 调用者已持域锁的纯槽准备；不取得执行权、不提交 IO、不发布消费者。
   */
  operation_token prepare_locked(io_request&& request,
                                 completion_target target,
                                 int& error) noexcept {
    operation_state* selected = nullptr;
    /// @brief 退休槽不再计为可用容量，也不计为 in-flight；本次继续查找其他安全槽。
    while (!free_.empty()) {
      const auto candidate_index = free_.back();
      free_.pop_back();
      auto& candidate = *operations_[candidate_index];
      if (candidate.generation == UINT32_MAX) {
        /// @brief 永久退休，禁止 generation 回绕与迟到 token 碰撞。
        ++retired_slots_;
        continue;
      }
      selected = &candidate;
      break;
    }
    if (!selected) {
      if (backend_.native()
          && (request.kind == operation_kind::close || request.internal_control)) {
        try {
          /// @brief 控制记录总数受资源/操作容量共同约束；不占用普通业务槽。
          /// @brief 关闭责任无法保存时不能泄漏或提前复用fd。
          if (native_controls_.size() >= max_resources_ + operations_.size()
              || next_control_ == UINT32_MAX)
            std::terminate();
          const auto token = (static_cast<std::uint64_t>(++next_control_) << 32) | UINT32_MAX;
          auto owned = std::make_unique<operation_state>();
          owned->slot = UINT32_MAX;
          selected = native_controls_.emplace(token, std::move(owned)).first->second.get();
          selected->token = {token};
        } catch (...) {
          /// @brief 无法拥有已接管fd时明确终止，禁止偷偷转线程仿真。
          std::terminate();
        }
      }
    }
    if (!selected) {
      /// @brief 所有未退休槽都在使用，调用方仍能在接受前安全回滚。
      error = EAGAIN;
      if (free_.size() + retired_slots_ == operations_.size())
        quiescent_cv_.notify_all();
      return {};
    }
    auto& op = *selected;
    const auto index = op.slot;
    /// @brief 控制记录已有不可复用token，普通槽仍使用代际协议。
    if (index != UINT32_MAX)
      ++op.generation;
    /// @brief 直接从调用者移入稳定槽，免去按值形参的中间请求移动。
    op.request = std::move(request);
    /// @brief 消费者仅由用户态统一发布链调用。
    op.target = target;
    if (index != UINT32_MAX)
      op.token = {static_cast<std::uint64_t>(op.generation) << 32 | (index + 1)};
    /// @brief 从此开始占用容量，直到结果发布完毕才允许回收。
    op.allocated = true;
    /// @brief Prepared 尚未执行 syscall，也没有资源 active 计数。
    op.accepted = false;
    op.terminal = false;
    /// @brief 服务未接受或调用已结束后，清除后台占用标记，允许统一终态处理。
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
    /// @brief 完成节点已被独占摘除，其他发布者不能再次领取同一责任。
    op.queued_completion = false;
    /// @brief 清除本节点反向链接，避免后续回收再次触碰已摘除队列。
    op.completed_previous = nullptr;
    op.result = 0;
    op.transferred = 0;
    /// @brief 清除前一次代际的原因，新的取消只允许首次写入。
    op.cancellation = 0;
    /// @brief 清除本节点完成链正向链接，下一代或独占发布不再沿用旧链。
    op.next = nullptr;
    op.observer_next = nullptr;
    return op.token;
  }

  /** @brief 调用者已持域锁的唯一接受状态转换；与原 submit 锁内语句保持一致。
   * @return false 只表示旧代际或已接受 token；true 表示本次取得接受责任。
   * @details 参数错误、deadline、关闭/方向 reservation、native/backpressure
   * 均复用此入口。
   */
  bool submit_locked(operation_token token,
                     bool& wake_needed,
                     bool& native_completion_opportunity) noexcept {
    /// @brief 必须同时检查 slot 与 generation，不能只凭地址找请求。
    auto* op = lookup(token);
    if (!op || op->accepted)
      return false;
    /// @brief 唯一接受边界；后续错误也必须向消费者交付一次结果。
    op->accepted = true;
    if (stopped() && op->request.kind != operation_kind::close && !op->request.uncancellable)
      finish(*op, -ECANCELED);
    else if (op->cancellation)
      finish(*op, -op->cancellation);
    else if (op->request.validation_error)
      /// @brief 非法参数不取得 IO 执行权，也不触发系统调用。
      finish(*op,
             -op->request.validation_error);

    else if (op->request.kind == operation_kind::ready
             && (op->request.argument <= 0 || op->request.argument > 3))
      /// @brief 原生 POLL_ADD 与 readiness adapter 使用同一 Interest 契约。
      finish(*op, -EINVAL);

    else if (op->request.kind == operation_kind::recvmsg && !op->request.output_message)
      /// @brief 在复制消息描述符前拒绝空输出指针。
      finish(*op, -EFAULT);
    else if (op->request.kind == operation_kind::recvmsg
             && static_cast<std::uint64_t>(op->request.output_message->msg_iovlen)
                    > native_iov_limit)
      finish(*op, -EINVAL);
    else if (op->request.kind == operation_kind::recvmsg && op->request.output_message->msg_iovlen
             && !op->request.output_message->msg_iov)
      finish(*op, -EFAULT);
    else if (op->request.kind == operation_kind::close && !op->request.resource
             && op->request.fd < 0)
      /// @brief 原始整数 close 不具有可幂等查询的共享身份。
      finish(*op, -EBADF);
    else {
      try {
        auto& req = op->request;
        if (!req.bypass_resource_registration && !req.resource && req.fd >= 0
            && req.kind != operation_kind::open && req.kind != operation_kind::open2)
          req.resource = borrow(req.fd);
        /// @brief 当前 allocated 槽拥有资源 lease；下述关闭复用只改字段，不替换 resource。
        const auto& r = req.resource;
        if (r && r->owner.get() != this)
          finish(*op, -EXDEV);
        else if (r && req.kind != operation_kind::close && (r->fd() < 0 || r->closing))
          finish(*op, -EBADF);
        else if (req.establish_reservation && !r)
          /// @brief 组合租约只能归属于稳定资源，不能凭 raw fd 建立。
          finish(*op,
                 -EBADF);
        else {
          if (r)
            req.fd = r->fd();
          if (req.deadline && *req.deadline <= std::chrono::steady_clock::now())
            finish(*op, -ETIMEDOUT);
          else if (req.kind == operation_kind::close && r) {
            r->owns_handle = true;
            begin_close(*r, op);
          } else {
            if (r) {
              const auto direction = direction_of(req.kind);
              if (req.establish_reservation
                  && (!req.reservation || (direction != 1 && direction != 2)))
                /// @brief 取消/validation/deadline 均已先仲裁，不留下空租约。
                finish(*op, -EINVAL);

              else if ((direction & 1
                        && (r->reader || (r->read_reserved && r->read_reserved != req.reservation)))
                       || (direction & 2
                           && (r->writer
                               || (r->write_reserved && r->write_reserved != req.reservation))))
                finish(*op, -EBUSY);
              else {
                if (req.establish_reservation) {
                  /// @brief 只读组合取得读方向预留；其余合法单方向组合取得写方向预留。
                  if (direction == 1)
                    r->read_reserved = req.reservation;
                  else
                    r->write_reserved = req.reservation;
                  /// @brief 原生 CQE/后续短写之间仍由组合帧保持排他。
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
                /// @brief 覆盖排队、syscall 及严格取消排空；close 必须等待归零。
                ++r->active;

                /// @brief finish 仅为实际取得的资源租约减一次计数。
                op->counts_active = true;
                if (req.kind == operation_kind::ready) {
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
              else if (unix_operations::is_file_request(req))
                submit_file(*op);
              else if (req.observed_would_block && r && r->registered
                       && req.observed_readiness_generation == r->readiness_generation
                       && !(r->readiness & (direction_of(req.kind) | error_bit))) {
                /// @brief 同一已注册 ET 资源、没有更新事件：真实 EAGAIN
                /// 已执行，无需再次 syscall。 新边沿尚在内核则下一 drive
                /// 会处理；已消费的更新边沿必改变 generation。closed 仅是信息，
                /// 对应 EOF/HUP 必须带本方向位，不能以相反方向半关闭强制重复 attempt。
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
    if (current_domain != this) {
      /// @brief 外域提交必须通知实际 owner，不能凭本请求终态省略通知。
      /// @brief 域锁内检查完整代际；此处尚未开始锁外发布。
      const auto* pending = lookup(token);
      /// @brief 本 token 之外的完成仍由 owner driver 发布；typed Cancel
      /// 可先终结原请求再终结自己。 head 是本 token 时，next
      /// 非空即证明还有另一槽；否则非空 head 自身就是另一 token。
      const bool other_completion =
          completed_head_ && (completed_head_->token.value != token.value || completed_head_->next);
      /// @brief 锁外 publish_one 只领取本 token，随后唤醒以交付其他完成。
      if ((pending && !pending->terminal) || other_completion)
        wake_needed = true;
    }
    /// @brief 复用 submit 已持有的域锁作 O(1) 资格快照，免去机会函数的额外首轮域锁。
    /// 快照不接管 CQ 责任；取消/远程提交改变状态时，driver
    /// 获锁后的二次校验仍拒绝。
    native_completion_opportunity = current_domain == this && !current_driver_domain
                                    && backend_.native() && can_complete_local_native(token);
    return true;
  }

  /** @brief 接受以后锁外处理低负载 native 机会、即时交付与远程控制通知。
   * @details 仅重用既有 SQE/CQE 和发布协议；不创建子协程、辅助队列或工作线程。
   */
  void finish_submission(operation_token token,
                         bool wake_needed,
                         bool native_completion_opportunity) noexcept {
    /// @brief 本地机会沿 driver → domain → backend 原锁序，只尝试一次真实 CQE。
    /// 当前 token 若终结，在 complete_native
    /// 同一个域锁内领取，免去再次锁定领取。
    const auto delivery =
        native_completion_opportunity ? try_complete_local_native(token) : claimed_completion{};
    /// @brief 成功机会函数已经退出它自己的 driver/session/domain
    /// 全部作用域，再发布消费者。
    /// @brief miss/锁失败/远程/readiness 保持原领取与竞态补查路径。
    if (delivery.operation)
      /// @brief 只发布本 token；其余完成仍留在原 owner 完成链。
      publish_claimed(delivery);
    else
      publish_one(token);
    /// @brief 远程新deadline/等待者登记打断无限poll，本地快路径不做唤醒syscall。
    if (wake_needed)
      wake();
  }

  /** @brief 在写方向排空后直接发原生SHUTDOWN；resource active覆盖最终CQE。 */
  void issue_shutdown_write(resource_state& resource) noexcept {
    if (resource.write_shutdown_issued || resource.closing || resource.fd() < 0)
      return;
    /// @brief 只接管一次半关闭责任。
    resource.write_shutdown_issued = true;
    if (!supports_native(operation_kind::shutdown)) {
      /// @brief readiness控制操作是短syscall。
      unix_operations::shutdown_write(resource.fd());
      return;
    }
    auto held = resources_[resource.id].lock();
    /// @brief 整个资源已在析构关闭边界，无需再增加半关闭。
    if (!held)
      return;
    io_request request;
    request.kind = operation_kind::shutdown;
    request.resource = std::move(held);
    request.argument = SHUT_WR;
    request.uncancellable = true;
    request.internal_control = true;
    /// @brief 保存准备阶段错误码，准备失败表示域尚未接管异步完成责任。
    int error{};
    const auto token = prepare(std::move(request), {}, error);
    if (!token.value)
      std::terminate();
    /// @brief 与普通native请求完全相同的SQE/CQE管线。
    submit(token);
  }

  /** @brief 原生扩展能力在接受前按 opcode/level/family 一次选择；绝不提交后重做
   * SET。 */
  bool can_native_request(const io_request& request) const noexcept {
    if (!supports_native(request.kind))
      return false;
    if (request.kind == operation_kind::getsockopt && request.argument != SOL_SOCKET)
      return false;
    if (request.kind == operation_kind::getsockopt || request.kind == operation_kind::setsockopt
        || request.kind == operation_kind::socket_inq
        || request.kind == operation_kind::socket_outq) {
      const int family = request.resource ? request.resource->socket_family : AF_UNSPEC;
      /// @brief Unix 没有对应 URING_CMD，使用等价短 syscall。
      if (family != AF_INET && family != AF_INET6)
        return false;
    }
    return true;
  }

  /** @brief 域锁内判断低负载原生机会，不扫描操作槽或增加热路径计数器。
   * @details listener ACCEPT 与一条连接可共用域；至多八个已分配槽和资源才触发。
   *          资源数门控保守保留多连接批处理，已有完成/控制/背压优先交给正常
   * driver。
   */
  bool can_complete_local_native(operation_token token) noexcept {
    const auto allocated = operations_.size() - free_.size() - retired_slots_;
    /// @brief 只使用已有容器大小，忙域和排空阶段不拆散提交批次。
    if (allocated > 8 || resources_.size() > 8 || !native_controls_.empty()
        || !native_pending_.empty() || completed_head_ || fatal_error_ || stopped())
      return false;
    /// @brief 重新核对完整代际，不复用已发布或回收的槽地址。
    auto* operation = lookup(token);
    return operation && operation->accepted && operation->native_inflight && !operation->terminal
           && !operation->request.internal_control && operation->target.publish;
    /// @brief 内置 SHUTDOWN/孤立 CLOSE 的 submit 可能仍在外层域锁内，不能尝试取得
    /// driver 锁。 只允许有业务消费者的普通原生请求；内部控制沿原批量 driver
    /// 管线排空。
  }

  /** @brief 本地低负载的一次原生提交/CQE 机会；不等待，不执行同步业务 syscall。
   * @param token submit 当前操作的完整代际，完成后仍由其原 publish_one 交付。
   * @details driver mutex 只 try_lock，未取得或不再符合条件即保持原挂起行为。
   *          poll(0) 内部已经 flush；禁止另做重复 flush 或循环等待 CQE。
   *          ACK/MORE/NOTIF 全部复用 complete_native 的租约与唯一终态协议。
   */
  claimed_completion try_complete_local_native(operation_token token) noexcept {
    /// @brief 远程请求、已有 CQ session 和 readiness 后端均保留原驱动方式。
    if (current_domain != this || current_driver_domain || !backend_.native())
      return {};

    /// 首次资格已在 submit 的原有域锁内快照；这里不重复取得域锁或等待 CQ
    /// 消费者。
    std::unique_lock driver_lock(driver_mutex_, std::try_to_lock);
    /// @brief 已有唯一消费者时不等待、不争抢其 CQ 完成责任。
    if (!driver_lock.owns_lock())
      return {};
    /// @brief 内部 close/callback 再 submit 时跳过机会。
    driver_session_guard session{this};
    int failure{};
    /// @brief 仅拥有领取责任的标量快照；不复制资源 lease。
    claimed_completion delivery;
    {
      /// @brief 锁序与正常 drive 一致：driver → domain → SQ。
      std::lock_guard lock(mutex_);
      /// @brief 两锁之间可能发生取消/远程提交，必须再次核对资格和代际。
      if (!can_complete_local_native(token))
        return {};
      /// @brief 单批真实 CQE 输出；只读取返回数量内的完整记录。
      std::array<backend_event, 8> events;
      /// @brief 一次原生 flush + 非阻塞真实 CQE 消费。
      const int count = backend_.poll(events, 0);
      if (count < 0)
        /// @brief 永久故障仍由原 drain/fail-fast 协议处理，不提前释放 buffer。
        failure = -count;

      else {
        /// @brief 所有真实结果进入同一状态机；ACK/MORE/NOTIF
        /// 保留原唯一终态和最终释放协议。
        for (int index = 0; index < count; ++index)
          complete_native(events[static_cast<std::size_t>(index)]);
        /// @brief 完成与领取共用原域锁；只摘本 token，稳定 request
        /// 仍由该槽拥有到发布返回。
        delivery = claim_completed_locked(token);
      }
    }
    /// @brief 域锁外处理停机回调；session 仍阻止驱动重入。
    if (failure)
      enter_failure(failure);
    /// @brief return 销毁 session 和 driver_lock 以后，finish_submission 才能执行未知 callback。
    return delivery;
  }

  /** @brief 永久后端故障先建立内核 drain 边界，不能把仍在途的 buffer
   * 当作失败结果发布。 */
  void enter_failure(int error) noexcept {
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      if (fatal_error_)
        return;
      fatal_error_ = error;
      /// @brief 不可证明 kernel release 时明确终止，拒绝静默悬挂/ UAF。
      if (!backend_.begin_failure(error))
        std::terminate();

      for (auto& entry : operations_)
        if (entry->allocated && entry->accepted && !entry->terminal
            && !entry->request.uncancellable)
          if (!entry->cancellation)
            entry->cancellation = error;
    }
    /// @brief 锁外发 stop 与外部 cleanup 回调，资源关闭仍等待原生最后引用。
    begin_shutdown();

  }

  /** @brief native accepted 与内核引用分离；SQ 满不会丢请求，也不在 worker
   * 阻塞。
   * @details pending 链最多操作池容量；请求始终在槽内，路径、iovec
   * 描述符地址稳定。
   */
  void submit_native(operation_state& op) noexcept {
    /// @brief 显式流式空请求不借用内核 buffer；datagram 不能套用这个成功规则。
    if (empty_stream_request(op.request)) {
      finish(op, 0);
      return;
    }
    /// @brief 请求已经在稳定槽内，后端接受后即承担 native lease，即使 SQ 尚未 enter。
    /// @brief 原生CLOSE责任始终保持到真实CQE，不转辅助线程。
    const auto result =
        fatal_error_
            ? backend_submit_result{backend_submit_status::rejected, fatal_error_}
            : backend_.try_submit(
                  {op.token.value, &op.request});
    if (result.status == backend_submit_status::accepted) {
      /// @brief 最终原请求 CQE 或 buffer release 前禁止 finish。
      op.native_inflight = true;
      /// @brief 已经转交 SQ 的请求不再占域 pending 队列责任。
      op.native_pending = false;
    } else if (result.status == backend_submit_status::would_queue
               || ((op.request.kind == operation_kind::close || op.request.internal_control)
                   && result.error == ENOMEM)) {
      /// @brief 只登记一次背压责任；普通请求 SQ 满与关闭控制接受前 ENOMEM
      /// 都可保留重试。
      if (!op.native_pending) {
        op.native_pending = true;
        try {
          /// @brief 保存完整代际而非槽指针，回收后迟到 pending 项不会命中新操作。
          native_pending_.push_back(op.token);
        } catch (...) {
          op.native_pending = false;
          /// @brief 控制责任不能以ENOMEM结果提前归还尚未关闭fd。
          if (op.request.kind == operation_kind::close || op.request.internal_control)
            std::terminate();
          /// @brief 普通请求尚未被 backend 接受，才允许分配失败形成安全终态。
          finish(op, -ENOMEM);
        }
      }
    } else {
      op.native_pending = false;
      if (op.request.kind == operation_kind::close) {
        /// @brief ENOMEM/SQ背压已保留重试；其余接受前永久错误无法合法丢弃关闭责任。
        std::terminate();
      } else
        /// @brief 原生接受前拒绝没有 kernel lease，负 errno 仍经统一终态发布。
        finish(op, -result.error);
    }
  }

  /** @brief 一批有界重试；CQ 消费后释放 SQ 容量，再把排队请求交给后端。 */
  void pump_native_pending() noexcept {
    /// @brief 每轮最多 256 次重试，不因 SQ 持续满载而在 worker 忙循环。
    const auto limit = std::min<std::size_t>(native_pending_.size(), 256);
    for (std::size_t i = 0; i < limit; ++i) {
      /// @brief 先取稳定代际，再弹出旧责任；重新背压时 submit_native 会放回队列。
      const auto token = native_pending_.front();
      native_pending_.pop_front();
      auto* op = lookup(token);
      /// @brief 已消费/取消或重复旧项只丢弃数值记录，不访问已复用请求。
      if (!op || op->terminal || !op->native_pending)
        continue;
      /// @brief 当前项暂由本轮负责，下一次 would_queue 才重新登记。
      op->native_pending = false;
      /// @brief 未交内核的普通取消可以直接完成，已接管关闭责任不能被取消跳过。
      if (op->cancellation && !op->native_close && !op->request.uncancellable)
        finish(*op, -op->cancellation);
      else
        submit_native(*op);
    }
  }

  /** @brief 取消 ACK 不解除 payload 租约；这里只要求内核取消，原 CQE 决定终态。
   */
  void cancel_operation(operation_state& op) noexcept {
    /// @brief 唯一 close 责任只允许真实完成。
    if (op.native_close || op.request.uncancellable)
      return;
    if (op.native_inflight) {
      if (!op.native_cancel_requested && !op.native_close) {
        /// @brief 一次发布取消责任，原请求仍保持 native_inflight 直到最后 CQE。
        op.native_cancel_requested = true;
        /// @brief 精确 token + 稳定 request 交后端，不按数字 fd 取消可能重用的资源。
        backend_.request_cancel({op.token.value, &op.request});
      }
    } else if (!op.running_file) {
      /// @brief 尚无内核/文件服务引用才可撤销待提交责任，running_file 必须等真实返回。
      op.native_pending = false;
      /// @brief 用已确定的停止原因完成当前操作，不能被后来就绪或成功结果覆盖。
      finish(op, -op.cancellation);
    }
  }

  /** @brief 原生结果与资源就绪在核心汇合；MORE 结果保留到 NOTIF 后才发布一次。
   */
  void complete_native(const backend_event& event) noexcept {
    /// @brief ACK 可早于原请求完成，不能回收 generation 或恢复用户。
    if (event.kind == backend_event_kind::cancel_ack)
      return;
    /// @brief 后端整数事件重新核对域 slot/generation，迟到业务 CQE 不能访问复用槽。
    auto* op = lookup(operation_token{event.key});
    if (!op || !op->native_inflight || op->terminal)
      return;
    if (event.kind == backend_event_kind::buffer_release) {
      if (!op->native_waiting_release)
        return;
      /// @brief 最终释放通知解除 payload lease；不能把 NOTIF 的 res 当成发送结果。
      op->native_waiting_release = false;
      op->native_inflight = false;
      /// @brief NOTIF 没有业务字节数，使用前一个发送 CQE 的结果。
      finish(*op,
             op->result);
      return;
    }
    /// @brief 保留真正业务 CQE 结果，后续取消仲裁只改变错误，不抹去已完成进度。
    op->result = event.result;
    /// @brief 中立 MORE 表示还有内核引用，结果先保存，绝不提前恢复借用数据的协程。
    if (event.flags & 1u) {
      op->native_waiting_release = true;
      return;
    }
    /// @brief 最终业务 CQE 已解除 native lease，统一 finish 才能登记唯一终态。
    op->native_inflight = false;
    if (op->native_close) {
      /// @brief CLOSE 的资源租约仍在本槽，所有 close waiter 只能在真实关闭结果后完成。
      auto resource = op->request.resource;
      if (resource) {
        resource->close_pending = false;
        finish_close_waiters(*resource, event.result < 0 ? static_cast<int>(-event.result) : 0);
      }
      if (!op->terminal)
        finish(*op, event.result);
      return;
    }
    if (op->request.kind == operation_kind::ready && event.result >= 0 && op->request.resource) {
      auto& resource = *op->request.resource;
      const auto flags = unix_operations::readiness_flags(event.result);
      /// @brief 只有显式原生 ready 请求在此生成观察代际，数据 IO 不转换成 readiness。
      ++resource.readiness_generation;
      /// @brief 保留方向和关闭信息；真实 EAGAIN 或同代际 guard 再清对应方向。
      resource.readiness |= flags;
      finish(*op, flags);
    } else {
      finish(*op, event.result);
    }
  }

  /// @brief 同时核对槽位、分配状态和完整代际；旧令牌不能访问复用后的操作。
  operation_state* lookup(operation_token token) noexcept {
    /// @brief 令牌低 32 位编码普通槽位加一；保留最大值用于独立原生控制记录。
    const auto index = static_cast<std::uint32_t>(token.value);
    /// @brief 控制令牌不占普通槽数组，必须在独立控制表内按完整值查找。
    if (index == UINT32_MAX) {
      /// @brief 使用不可复用的完整控制键查找稳定控制状态。
      const auto found = native_controls_.find(token.value);
      /// @brief 未登记的控制键不产生任何取消或完成副作用。
      return found == native_controls_.end() ? nullptr : found->second.get();
    }
    /// @brief 先拒绝零值与越界槽号，再访问预分配操作数组。
    if (!index || index > operations_.size())
      return nullptr;
    /// @brief 把从一开始的令牌槽号还原为数组下标，节点地址在整个域生命期内稳定。
    auto* op = operations_[index - 1].get();
    /// @brief 必须同时匹配已分配状态与完整代际，数值槽号相同不足以证明是同一操作。
    return op->allocated && op->token == token ? op : nullptr;
  }

  /** @brief 一个资源只安装一次持久 readiness 注册，避免逐次 MOD/rearm 成本。 */
  int ensure_registration(resource_state& resource) noexcept {
    /// @brief 已有持久就绪登记时复用该登记，避免逐次修改内核兴趣集。
    if (resource.registered)
      return 0;
    /// @brief 普通文件不能套用 socket 或管道的 readiness 执行路径。
    if (resource.regular_file)
      return -ENOTSUP;
    /// @brief 把原生句柄与单调资源键关联到后端，事件不携带协程地址。
    const int result = backend_.attach(resource.fd(), resource.id);
    /// @brief 仅成功登记后才允许发布 registered 标志。
    if (!result)
      /// @brief 固定持久登记状态，后续暂不可用的操作沿用同一内核注册。
      resource.registered = true;
    return result;
  }

  /** @brief readiness adapter 先尝试真实 IO，仅 EAGAIN 才保留挂起状态。 */
  void attempt(operation_state& op) noexcept {
    if (op.terminal || op.running_file
        /// @brief 文件 job 拥有请求，driver 不得同时触碰其缓冲区。
        || op.native_inflight)
      return;
    /// @brief 嵌套 finish 只登记完成链，当前 stable 槽在本函数返回以前不会回收。
    const auto& resource = op.request.resource;
    /// @brief 首次停止原因已固定时，在尚未执行系统调用前直接转入取消终态。
    if (op.cancellation) {
      /// @brief 用已确定的停止原因完成当前操作，不能被后来就绪或成功结果覆盖。
      finish(op, -op.cancellation);
      return;
    }
    /// @brief 就绪观察只报告方向状态，不取得或返回数据传输的字节结果。
    if (op.request.kind == operation_kind::ready) {
      /// @brief 读取调用方请求观察的读写兴趣集合，后续校验只接受合法方向位。
      const auto mask = static_cast<std::uint32_t>(op.request.argument);
      /// @brief 观察必须有合法非空方向与稳定资源，否则直接生成参数错误。
      if (!mask || mask > 3 || !resource) {
        /// @brief 为已接受请求记录参数错误，并沿统一完成发布链交付一次。
        finish(op, -EINVAL);
        return;
      }
      if (const int e = ensure_registration(*resource); e) {
        finish(op, e);
        return;
      }
      /// @brief 半关闭附加信息不能替代 Interest；对端仍可在另一个方向发送响应。
      if (resource->readiness & (mask | error_bit))
        finish(op, resource->readiness);
      return;
    }
    if (op.request.kind == operation_kind::cancel) {
      if (static_cast<unsigned>(op.request.flags) & ~3u) {
        /// @brief ALL=1/FD=2 可等价；FD_FIXED/ANY 需要额外身份，不能假装支持。
        finish(op, -ENOTSUP);

        return;
      }
      if (resource) {
        /// @brief 本资源最多一个reader/writer并可有多个observer/元数据；统一按代际请求逐一取消。
        /// Cancel成功只表示控制请求已接受，借用buffer仍须等待各原请求的最终完成。
        for (auto& entry : operations_)
          if (entry.get() != &op && entry->allocated && entry->accepted && !entry->terminal
              && (entry->request.resource == resource
                  || (entry->request.bypass_resource_registration && !entry->request.resource
                      && entry->request.fd == op.request.fd))
              && entry->request.kind != operation_kind::close && !entry->request.uncancellable) {
            /// @brief File 原生请求由外层 active lease 固定 fd；只匹配本域明确 bypass
            /// 的借用请求。 普通资源仍比较共享身份，绝不凭相同整数 fd
            /// 取消另一代已重用资源。
            if (!entry->cancellation)
              entry->cancellation = ECANCELED;
            /// @brief 原生ACK与original CQE各自排空，不借控制成功提前恢复业务。
            cancel_operation(*entry);

          }
      }
      finish(op, 0);
      return;
    }
    if (op.request.kind == operation_kind::sendmsg)
      op.request.message.msg_iov = op.request.vectors.data();
    /// @brief 在已取得执行权的非阻塞资源上执行一次真实系统调用，连接阶段状态跨重试保留。
    const auto result = unix_operations::perform(op.request, op.connecting);
    if (result == -EAGAIN || result == -EWOULDBLOCK || result == -EINPROGRESS
        || result == -EALREADY) {
      if (!resource) {
        /// @brief 真实系统调用已经安全返回，以其短传输、EOF 或错误建立本次完成。
        finish(op, result);
        return;
      }
      /// @brief 真实 EAGAIN 才清除对应就绪提示。
      resource->readiness &= ~direction_of(op.request.kind);
      if (const int error = ensure_registration(*resource); error)
        finish(op, error);
      return;
    }
    /// @brief 真实系统调用已经安全返回，以其短传输、EOF 或错误建立本次完成。
    finish(op, result);
  }

  /** @brief 有界文件 lane 接受 stable token，取消也要等 job
   * 真实结束才释放租约。 */
  void submit_file(operation_state& op) noexcept {
    /// @brief 排队阶段也禁止 slot/buffer 回收，cancel 只记录 sticky 原因。
    op.running_file = true;
    /// @brief 用共享域租约覆盖服务线程执行与最后完成通知，避免借用已销毁的 worker。
    auto self = shared_from_this();
    /// @brief 后台工作只捕获完整代际身份，真正执行时在域锁内重新核对稳定槽。
    const auto token = op.token;
    const auto submitted = fs_->try_submit([self, token] {
      operation_state* operation;
      {
        /// @brief job 真正开始与 cancel 使用同一串行边界。
        std::lock_guard lock(self->mutex_);
        /// @brief accepted 租约覆盖排队、执行和完成，槽不能提前复用。
        operation = self->lookup(token);
        if (operation->cancellation) {
          /// @brief 未开始 syscall 的取消不产生文件写入/open 副作用；close 由独立清理
          /// lane 推进。
          operation->running_file = false;
          self->finish(*operation, -operation->cancellation);
          /// @brief 唤醒所属域驱动器，使后台结果进入原有锁外发布与资源排空流程。
          self->wake();
          return;
        }
      }
      /// @brief 为本次系统调用建立连接握手阶段状态，普通非连接请求不会使用该状态。
      bool connecting = false;
      /// @brief 释放状态锁后执行阻塞 syscall。
      const auto result = unix_operations::perform(operation->request,
                                                   connecting);
      {
        /// @brief 后台完成方与提交、取消共用同一域锁，防止终态与资源释放交错。
        std::lock_guard lock(self->mutex_);
        /// @brief 真实文件调用已返回或已在执行前取消，解除禁止槽回收的后台执行标记。
        operation->running_file = false;
        /// @brief 若 open 已创建 handle 但取消获胜，关闭副作用，不能泄漏新 fd。
        self->finish(*operation, result);
      }
      /// @brief 唤醒所属域驱动器，使后台结果进入原有锁外发布与资源排空流程。
      self->wake();
    });
    /// @brief 阻塞服务未接受任务时，不保留后台执行责任，立即按背压错误完成。
    if (!submitted) {
      /// @brief 服务未接受或调用已结束后，清除后台占用标记，允许统一终态处理。
      op.running_file = false;
      /// @brief 把服务拒绝错误转为已接受 IO 的失败完成，不丢弃等待者恢复责任。
      finish(op, -submitted.error().value());
    }
  }

  /** @brief terminal 只发布一次；阻塞 job 结束前不准进入 terminal。 */
  void finish(operation_state& op, std::int64_t result) noexcept {
    if (op.terminal || op.running_file
        /// @brief 文件 job 拥有请求，driver 不得同时触碰其缓冲区。
        || op.native_inflight)
      return;
    if (op.cancellation && result >= 0
        && ((op.request.kind == operation_kind::accept
             || op.request.kind == operation_kind::accept_nowait)
            || op.request.kind == operation_kind::open || op.request.kind == operation_kind::open2
            || op.request.kind == operation_kind::socket)) {
      /// @brief 取消获胜但内核已创建 fd：先接管新 fd
      /// 的唯一清理责任，不能泄漏或交用户复用。
      const int fd = static_cast<int>(result);
      /// @brief 取消后的新fd仍走原生CLOSE。
      if (!defer_native_close(fd))
        defer_cleanup(
            [fd] { (void)unix_operations::close_descriptor(fd); });
    }
    /// @brief 唯一终态转换，从此不允许 syscall 重试或重复加入完成链。
    op.terminal = true;
    /// @brief 读写结果单位是字节，ACCEPT 的 fd 和 CONNECT 的状态绝不是传输量。
    op.transferred = result > 0 && direction_of(op.request.kind)
                             && op.request.kind != operation_kind::accept
                             && op.request.kind != operation_kind::accept_nowait
                             && op.request.kind != operation_kind::connect
                         ? static_cast<std::uint64_t>(result)
                         : 0;
    /// @brief CQE成功已经删除一条；取消获胜也必须保留真实条目进度。
    if (op.request.kind == operation_kind::unlinkat && result == 0)
      op.transferred = 1;
    if (op.request.kind == operation_kind::ready && result >= 0 && op.request.resource)
      /// @brief ready 结果的 progress 是观察代际，供 guard 清理同一批提示而非字节。
      op.transferred = op.request.resource->readiness_generation;
    /// @brief 首次取消原因稳定，实际传输量另行保留。
    op.result =
        op.cancellation ? -op.cancellation : result;
    /// @brief 借用 stable 槽已有 lease；本函数不发布当前消费者，发布返回后才回收此槽。
    const auto& resource = op.request.resource;
    if (resource) {
      /// @brief 只解除本操作真正取得的方向 gate，不清其他代际的请求或 reservation。
      if (op.uses_reader && resource->reader == &op)
        resource->reader = nullptr;
      if (op.uses_writer && resource->writer == &op)
        resource->writer = nullptr;
      /// @brief 多观察者独立摘链，不消费业务数据或撤销其他 ready waiter。
      if (op.observer) {
        auto** link = &resource->observers;
        /// @brief 稳定槽地址只在域锁内作为链节点身份，内核和取消控制仍使用整数 token。
        while (*link && *link != &op)
          link = &(*link)->observer_next;
        /// @brief 找到本节点才重连后继，未登记的操作不改观察者链。
        if (*link)
          *link = op.observer_next;
      }
      /// @brief active 只对应已经登记方向/observer/普通资源请求，关闭 waiter 不占 active。
      if (op.counts_active)
        --resource->active;
      /// @brief 单次写完成不等于 write_all 完成：方向租约覆盖各 partial write 间隙。
      if (resource->write_shutdown && !resource->writer && !resource->write_reserved
          && resource->fd() >= 0)
        issue_shutdown_write(*resource);
    }
    /// @brief 双向节点允许 submit O(1) 领取自己的即时结果。
    op.completed_previous = completed_tail_;
    /// @brief 完成链暂时接管发布责任，槽和 payload 参数仍保留到锁外 callback 返回。
    op.next = nullptr;
    /// @brief publish_one/publish_completed 用此标记领取一次责任，禁止重复交付结果。
    op.queued_completion = true;
    /// @brief O(1) 追加完成双向链，submit 可仅摘自己的即时终态而不发布远程完成。
    if (completed_tail_)
      completed_tail_->next = &op;
    else
      completed_head_ = &op;
    /// @brief 完成责任仍只有一次，结果消费者始终在锁外调用。
    completed_tail_ = &op;
    /// @brief 最后 active 退出才接管真实关闭；closing 早已拒绝后续 IO。
    if (resource && resource->closing && !resource->active)
      close_handle(*resource);
    else if (resource && stopped() && draining_ && !resource->active)
      begin_close(*resource, nullptr);
  }

  /** @brief closing 的发布拒绝后续 IO，原先接受的请求被取消并严格排空。 */
  void begin_close(resource_state& resource, operation_state* waiter) noexcept {
    if (waiter) {
      /// @brief 已接管的close waiter不能先回收，否则链会保留复用槽地址。
      waiter->request.uncancellable =
          true;
      waiter->observer_next = resource.close_waiter;
      resource.close_waiter = waiter;
    }
    /// @brief 先发布 closing，后续 syscall 无法跨关闭边界取得执行权。
    resource.closing = true;
    /// @brief fsync 等无读写方向操作也必须参与关闭取消，不能只取消 reader/writer。
    for (auto& entry : operations_)
      if (entry->allocated && entry->accepted && !entry->terminal && entry.get() != waiter
          && entry->request.resource.get() == &resource
          && entry->request.kind != operation_kind::close)
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
    /// @brief 逐个检查该资源上的就绪观察者，观察者不消耗读写数据。
    while (observer) {
      auto* next = observer->observer_next;
      cancel_operation(*observer);
      /// @brief 使用提前保存的后继继续遍历，不能依赖可能已摘除节点的链接。
      observer = next;
    }
    if (!resource.active)
      close_handle(resource);
  }

  /** @brief 方向/操作租约排空后唯一接管 fd，native CLOSE 等真实 CQE
   * 再完成等待者。
   * @details handle.exchange(-1) 拒绝新 IO，但不代表内核 close 已完成；不能重复
   * close。
   */
  void close_handle(resource_state& resource) noexcept {
    if (resource.close_pending)
      return;
    /// @brief 唯一接管原生关闭责任。
    const int fd =
        resource.handle.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
      /// @brief 已有持久就绪登记时复用该登记，避免逐次修改内核兴趣集。
      if (resource.registered)
        backend_.detach(fd);
      resource.registered = false;
      descriptors_.erase(fd);
      if (resource.owns_handle) {
        if (supports_native(operation_kind::close)) {
          auto held = resources_[resource.id].lock();
          auto* operation = resource.close_waiter;
          if (!operation && held) {
            io_request request;
            request.kind = operation_kind::close;
            request.resource = held;
            request.bypass_resource_registration = true;
            /// @brief 保存准备阶段错误码，准备失败表示域尚未接管异步完成责任。
            int error{};
            const auto token = prepare(std::move(request), {}, error);
            operation = lookup(token);
            if (operation)
              operation->accepted = true;
          }
          if (operation) {
            resource.close_pending = true;
            operation->request.fd = fd;
            /// @brief 已接管 fd，必须等 CLOSE CQE，不可取消。
            operation->native_close = true;
            operation->request.uncancellable = true;
            operation->cancellation = 0;
            submit_native(*operation);
            return;
          }
        }
        if (unix_operations::needs_deferred_close(resource, fd)) {
          /// @brief 文件或正 SO_LINGER 的 close 可能阻塞，waiter 等 cleanup
          /// 真正完成才恢复。
          auto held = resources_[resource.id].lock();
          if (held) {
            resource.close_pending = true;
            defer_cleanup([held, fd] {
              const int result = unix_operations::close_descriptor(fd);
              const int error = result < 0 ? errno : 0;
              {
                std::lock_guard lock(held->owner->mutex_);
                held->close_pending = false;
                held->owner->finish_close_waiters(*held, error);
              }
            });
            return;
          }
        }
        (void)unix_operations::close_descriptor(fd);
      }
    }
    finish_close_waiters(resource, 0);
  }

  /// @brief 摘除关闭等待链，并让各等待操作取得同一关闭结果。
  void finish_close_waiters(resource_state& resource, int error) noexcept {
    auto* waiter = std::exchange(resource.close_waiter, nullptr);
    while (waiter) {
      auto* next = waiter->observer_next;
      finish(*waiter, error ? -error : 0);
      waiter = next;
    }
  }

  /** @brief 锁内摘除一个完成责任；摘链后仍 allocated，直到 publish 返回才回收。
   */
  void remove_completed(operation_state& op) noexcept {
    /// @brief 有前驱时让前驱绕过当前完成节点，保持双向完成链连通。
    if (op.completed_previous)
      /// @brief 修复前驱的正向链接，完成发布责任已由当前调用者独占领取。
      op.completed_previous->next = op.next;
    else
      /// @brief 当前节点为链头时推进域完成队列头。
      completed_head_ = op.next;
    /// @brief 后继存在时修复其反向链接；没有后继时当前节点原为队尾。
    if (op.next)
      /// @brief 将后继的前驱改为当前节点原前驱，支持后续常数时间摘链。
      op.next->completed_previous = op.completed_previous;
    else
      /// @brief 摘除队尾后把前驱提升为新尾，单节点队列同时变空。
      completed_tail_ = op.completed_previous;
    /// @brief 清除本节点完成链正向链接，下一代或独占发布不再沿用旧链。
    op.next = nullptr;
    /// @brief 清除本节点反向链接，避免后续回收再次触碰已摘除队列。
    op.completed_previous = nullptr;
    /// @brief 完成节点已被独占摘除，其他发布者不能再次领取同一责任。
    op.queued_completion = false;
  }

  /** @brief 锁内释放结果和资源租约，然后才开放下一代 slot。 */
  void recycle_completed(operation_state& op) noexcept {
    /// @brief 完成回调已经返回后才撤销分配状态，迟到令牌从此无法查到本代。
    op.allocated = false;
    /// @brief 清除本代接纳状态，槽归还后才能由下一代重新接管完成责任。
    op.accepted = false;
    /// @brief 消费者交付已结束，清除借用目标以免复用槽残留旧等待器地址。
    op.target = {};
    if (op.slot == UINT32_MAX) {
      /// @brief 独立控制槽保持原有完整清理顺序，再销毁节点；本优化只针对可复用普通槽。
      op.request = io_request{};
      native_controls_.erase(op.token.value);
      return;
    }
    /// @brief 最终 native release/文件 job 已结束，且 callback
    /// 已返回，才释放本代拥有参数。 空容器 swap 实际释放容量，不能 clear
    /// 后长期保留很大的路径或 iovec 存储。
    /// @brief 与原 request={} 相同，此时解除稳定资源租约。
    op.request.resource.reset();
    /// @brief 描述数组拥有结束，payload 从来只是借用。
    std::vector<iovec>{}.swap(op.request.vectors);
    /// @brief 空字符串接管旧动态块，在本语句末释放。
    std::string{}.swap(op.request.path);
    /// @brief 第二路径同样不保留上一代容量。
    std::string{}.swap(op.request.path2);
    /// @brief 其余字段仍保持合法已初始化表示，但 !allocated 的槽不允许读取它们发起 IO。
    /// 下一次 prepare_locked 先完整移动赋值 request，再发布 allocated=true；
    /// lookup/取消/关闭/deadline 扫描也都先校验
    /// allocated，因此不继承旧控制状态。
    /// @brief 原请求已解除全部拥有责任，现在才允许下一代领取。
    free_.push_back(op.slot);
  }

  /** @brief 域锁内独占领取本 token 完成；不回调、不回收、不移动稳定请求。
   * @return 空快照表示未完成或已经由其他 publisher 领取，不能重复交付。
   */
  claimed_completion claim_completed_locked(operation_token token) noexcept {
    /// @brief 校验 allocated、slot 与完整 generation。
    auto* operation = lookup(token);
    /// @brief driver 已领取时由它负责发布，不能重复交付。
    if (!operation || !operation->queued_completion)
      return {};
    /// @brief 从完成链独占摘除，仍 allocated 到 callback 返回。
    remove_completed(*operation);
    /// @brief callback 以前复制原目标/结果；未知消费者可在 callback 中销毁自己的 awaiter。
    return {operation, operation->target, operation->result, operation->transferred};
  }

  /** @brief 锁外交付已领取快照，callback 返回以后沿原域锁回收稳定槽。
   * @details 本地成功机会退出其 driver/session/domain
   * 后才调用；其他入口保持原作用域合同。
   */
  void publish_claimed(claimed_completion delivery) noexcept {
    /// @brief 原 arming gate 同步观察 completed。
    if (delivery.target.publish)
      delivery.target.publish(delivery.target.consumer,
                              delivery.result,
                              delivery.transferred);
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      /// @brief callback 返回后才回收稳定请求，下一代槽不能在发布者仍读取参数时复用。
      recycle_completed(*delivery.operation);
      /// @brief 完全保留原回收后的控制面排空通知。
      if (free_.size() + retired_slots_ == operations_.size())
        quiescent_cv_.notify_all();
    }
  }

  /** @brief 只交付 submit 当前 token 的即时完成，其他完成留给其 owner driver。
   */
  void publish_one(operation_token token) noexcept {
    claimed_completion delivery;
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      /// @brief 原正常入口复用完全相同的唯一领取协议。
      delivery = claim_completed_locked(token);
    }
    /// @brief 原锁外 callback 与回锁 recycle 顺序不变。
    if (delivery.operation)
      publish_claimed(delivery);
  }

  /** @brief driver 每次领取最多 64 个责任、锁外发布，再一次批量回收。
   * @details 消费者可以重入或恢复后销毁
   * awaiter；全部输出先复制，槽只在回调返回后复用。 callbacks
   * 新增的完成仍挂在域链表，后续批次继续处理，预算限制保持不变。
   */
  std::size_t publish_completed(std::size_t budget) noexcept {
    struct delivery {
      operation_state* operation;  ///< 地址稳定的底层异步操作，当前包装负责其启动、等待与结果交付。
      void* consumer;  ///< 借用的完成消费者地址，必须存活到唯一结果发布结束。

      void (*publish)(void*, std::int64_t, std::uint64_t) noexcept;  ///< 发布结果及字节进度的静态回调，调用后消费者可立即销毁。

      std::int64_t result;  ///< 已完成操作的返回值；具体错误解释遵循所属接口约定。
      std::uint64_t transferred;  ///< 操作已完成的字节数，失败或取消时仍保留有效进度。
    };

    /// @brief 每个领取槽都完整写出全部字段；只访问 count 内记录，不清零空批次。
    /// 将输出字段独立保存，避免 completion_target 的默认成员初始化清零未使用槽。
    /// @brief 固定栈批次，不分配任何容器或修改公开 target 默认值。
    std::array<delivery, 64> batch;
    /// @brief 累计当前驱动调用已交付的完成数，后续批次受剩余预算限制。
    std::size_t published = 0;
    /// @brief 只在仍有完成预算时领取下一批，避免完成回调不断新增工作导致驱动失去公平性。
    while (published < budget) {
      /// @brief 本批只读取已经完整写入的快照槽，不初始化未使用的栈数组尾部。
      std::size_t count = 0;
      {
        /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
        std::lock_guard lock(mutex_);
        /// @brief 批次同时受固定栈容量与本轮剩余完成预算限制。
        const auto limit = std::min(batch.size(), budget - published);
        /// @brief 在域锁内逐个独占摘除当前完成链前缀，其他发布者无法重复领取。
        while (count < limit && completed_head_) {
          /// @brief 保存当前完成节点；回调结束前保持 allocated 以防槽位被复用。
          auto* operation = completed_head_;
          /// @brief 锁内取得独占发布责任，其他 publisher 不能重复领取。
          remove_completed(*operation);

          batch[count++] = {operation,
                            operation->target.consumer,
                            operation->target.publish,
                            operation->result,
                            operation->transferred};
        }
        /// @brief 本批没有业务完成时只推进必要清理重试，然后结束本次完成领取。
        if (!count) {
          /// @brief 仅清理 lane 饱和时有工作，不新增普通空批次锁。
          flush_deferred_cleanup();

          break;
        }
      }
      for (std::size_t i = 0; i < count; ++i) {
        /// @brief 之后 callback 可销毁 consumer，不能再访问其 awaiter。
        const auto entry = batch[i];
        /// @brief 只有带消费者的完成才调用结果入口，控制记录可仅承担资源清理责任。
        if (entry.publish)
          /// @brief 在域锁外发布复制好的结果，回调可以恢复并销毁消费者自身。
          entry.publish(entry.consumer, entry.result, entry.transferred);
      }
      {
        /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < count; ++i)
          /// @brief 全部消费者调用已返回后，在域锁内逐个释放参数并归还槽位。
          recycle_completed(*batch[i].operation);
        flush_deferred_cleanup();
        if (free_.size() + retired_slots_ == operations_.size())
          quiescent_cv_.notify_all();
      }
      /// @brief 把已交付批次计入本次驱动预算，未交付结果留在原完成链。
      published += count;
    }
    /// @brief 返回实际交付数量，宿主据此判断本轮是否产生协程进展。
    return published;
  }

  /** @brief 域锁内把持久清理队列转交专属 lane，永不在 IO worker 执行 close。 */
  void flush_deferred_cleanup() noexcept {
    while (!deferred_cleanup_.empty()) {
      auto job = deferred_cleanup_.front();
      /// @brief 用共享域租约覆盖服务线程执行与最后完成通知，避免借用已销毁的 worker。
      auto self = shared_from_this();
      auto result = cleanup_->try_submit([self, job] {
        (*job)();
        if (self->publishers_.fetch_sub(1, std::memory_order_acq_rel) == 1)
          self->quiescent_cv_.notify_all();
        /// @brief 唤醒所属域驱动器，使后台结果进入原有锁外发布与资源排空流程。
        self->wake();
      });
      /// @brief 仅成功登记后才允许发布 registered 标志。
      if (!result)
        break;
      deferred_cleanup_.pop_front();
    }
  }

  /// @brief 把更早的操作绝对截止时间并入本域最近截止时间缓存。
  void update_deadline(std::chrono::steady_clock::time_point time) noexcept {
    if (!next_deadline_ || time < *next_deadline_)
      next_deadline_ = time;
  }

  /// @brief 扫描到期稳定操作，固定超时原因并按当前执行阶段推进安全取消。
  void expire_deadlines() noexcept {
    /// @brief 没有任何登记的 IO deadline 时不读取时钟；域锁保持判断与登记互斥。
    if (!next_deadline_)
      return;

    /// @brief 只有真实 deadline 才检查到期时间。
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
        /// @brief 已接受的 user/close/shutdown 取消原因优先保留。
        if (!op.cancellation)
          op.cancellation = ETIMEDOUT;
        cancel_operation(op);
      } else
        update_deadline(*op.request.deadline);
    }
  }

  backend_box backend_;  ///< 域唯一拥有的静态后端包装，负责原生提交、轮询与控制唤醒。
  std::deque<operation_token> native_pending_;  ///< 等待原生提交容量的操作令牌 FIFO，不保存可失效的槽位指针。
  std::unordered_map<std::uint64_t, std::unique_ptr<operation_state>> native_controls_;  ///< 独立于普通操作池的原生控制状态，覆盖异步关闭等清理责任。
  std::uint32_t next_control_{};  ///< 独立控制代际不复用；不侵占业务token高位。
  const std::size_t max_resources_;  ///< 同一 IO 域允许登记的长期资源数量上限。
  mutable std::recursive_mutex mutex_;  ///< 资源租约释放可能在当前状态转换里重入注销，因此使用可重入控制面锁。
  std::mutex driver_mutex_;  ///< 串行化后端驱动，防止多个线程同时消费同一完成队列。
  std::condition_variable_any quiescent_cv_;  ///< 关闭方等待业务操作、发布者和清理责任全部排空的条件变量。
  std::vector<std::unique_ptr<operation_state>> operations_;  ///< 预分配且地址稳定的操作池，内核解除引用后才允许复用槽位。
  std::vector<std::uint32_t> free_;  ///< 可复用操作槽或条目的集合，内核解除引用后才允许归还。
  std::size_t retired_slots_{};  ///< generation 用尽的槽，既非可用容量也非在途操作。
  std::unordered_map<std::uint64_t, std::weak_ptr<resource_state>> resources_;  ///< 资源键到弱资源租约的索引，迟到事件须核对单调资源身份。
  std::unordered_map<int, std::weak_ptr<resource_state>> descriptors_;  ///< 原生句柄到弱资源租约的索引，用于复用既有资源归属。
  std::uint64_t next_resource_{};  ///< 下一个单调资源身份，零保留给后端唤醒通道。
  operation_state* completed_head_{};  ///< 域内等待锁外发布的完成链表头。
  operation_state* completed_tail_{};  ///< 完成链表尾，追加完成时无需分配或遍历。
  std::optional<std::chrono::steady_clock::time_point> next_deadline_;  ///< 当前操作最早的绝对截止时间，驱动据此限制内核等待时长。
  std::atomic<bool> stopped_{};  ///< 停止状态的跨线程发布标志，后续接纳和查询据此拒绝新工作。
  int fatal_error_{};  ///< 首次永久驱动故障；在内核引用排空前禁止结果回收。
  bool draining_{};  ///< 域是否处于排空阶段，已接管的完成责任仍须履行。
  std::stop_source stop_;  ///< 本操作或任务图的停止源，向已登记等待者广播协作取消。
  std::shared_ptr<execution::blocking_executor> fs_,  ///< 共享文件阻塞服务，原生后端仅在实际回退时按需启动线程。
      dns_,  ///< 共享地址解析服务，避免阻塞协程工作线程。
      cleanup_;  ///< 共享关闭清理服务，不能因普通请求容量不足丢弃清理责任。
  bool own_fs_{},  ///< 文件服务是否由本域创建，决定析构时是否负责关闭。
      own_dns_{},  ///< 解析服务是否由本域创建，决定析构时是否负责关闭。
      own_cleanup_{};  ///< 清理服务是否由本域创建，决定析构时是否负责关闭。
  std::shared_ptr<io_placement_group> placement_;  ///< 同一运行时多个 IO 域之间的资源放置服务租约。
  std::atomic<std::size_t> publishers_{};  ///< 仍在锁外发布结果的线程数，域静止必须等待它们退出。
  std::unordered_map<std::uint64_t, ::faio::move_only_function<void()>> shutdown_cleanups_;  ///< 停机时必须执行的拥有型资源清理回调表。
  std::uint64_t next_cleanup_{};  ///< 单调分配的停机清理登记编号。
  std::deque<std::shared_ptr<::faio::move_only_function<void()>>> deferred_cleanup_;  ///< 服务暂时饱和时保留的清理任务，后续驱动负责重试。
};

}

#endif
