#pragma once
/** @file domain.hpp @brief Windows IO
 * 域适配：复用稳定槽、资源租约、单消费者与锁外完成发布协议。 */
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/backends/factory.hpp"
#include "faio/detail/io/capabilities.hpp"
#include "faio/detail/io/core/resource_state.hpp"
#include "faio/detail/io/platform/windows_file.hpp"
#include "faio/detail/io/platform/windows_socket.hpp"
#include "faio/detail/io/reactor/readiness_adapter.hpp"
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
/** @brief runtime 控制面共享的弱 shard 注册表，用于首次分配与 raw fd 兼容查找。
 * @details weak_ptr 不使 domain 与 runtime 形成强引用环；选择在短锁内轮转，
 *          普通已注册 IO 热路径不访问此表，也不迁移资源。
 */
class io_placement_group {
 public:
  explicit io_placement_group(std::size_t count) : domains_(count) {}

  void register_domain(std::size_t index, const std::shared_ptr<io_domain>& domain) noexcept {
    std::lock_guard lock(mutex_);
    domains_[index] = domain;  // 启动屏障前登记，停止后弱租约不会访问 worker 地址。
  }

  std::shared_ptr<io_domain> select() noexcept;

  /** @brief 在同一 runtime 内查找已有 fd 身份，不建立新的资源或原生请求。
   * @param fd 调用方仍持有的原生描述符。
   * @param skipped 已查询过的本域，避免再次取得同一域锁。
   * @return 固定原归属的资源租约；未注册时返回空，不跨独立 engine 查找。
   */
  resource_ptr find_resource(native_descriptor fd, const io_domain* skipped) noexcept;

 private:
  std::mutex mutex_;
  std::vector<std::weak_ptr<io_domain>> domains_;
  std::size_t next_{};
};

struct immediate_result {
  std::int64_t result{};
  std::uint64_t transferred{};
};
}  // namespace faio::io::detail

namespace faio::io {
/** @brief 独立引擎配置；runtime 可注入共享隔离服务，避免每个 worker
 * 创建线程池。 */
struct engine_config {
  detail::backend_box (*backend_factory)(){};  ///< 中立后端工厂，支持合同测试及外部后端。
  unsigned native_queue_entries{
      256};  ///< 中立配置兼容字段；IOCP 没有 SQ，稳定池与操作容量分别有界。
  detail::reactor_box (*reactor_factory)(){};  ///< 可选自定义/测试后端工厂，仍遵守同一状态协议。
  std::size_t max_operations{4096}, max_resources{65536};
  std::size_t filesystem_threads{2}, resolver_threads{1},
      blocking_queue_limit{4096};  ///< 独立服务线程和等待任务上限；原生数据请求不进入它们。
  std::shared_ptr<execution::blocking_executor> filesystem_service, resolver_service,
      cleanup_service;
  std::shared_ptr<detail::io_placement_group> placement_service;  ///< 新连接控制面共享弱 shard 表。
};

namespace detail {
inline thread_local io_domain* current_domain{};
/// @brief 当前线程的 CQ 驱动 session；与业务 coroutine 的 current_domain
/// 分开保存。
inline thread_local io_domain* current_driver_domain{};

/** @brief 只维护 CQ 消费者的线程内防重入纪律，不拥有请求或原生后端。
 * @details 已有 session 时调用者应直接跳过新 session；避免同线程再次 try_lock
 *          已持有的非递归 driver mutex。析构恢复原值，不改变业务资源归属。
 */
class driver_session_guard {
 public:
  explicit driver_session_guard(io_domain* domain) noexcept
      : previous_(std::exchange(current_driver_domain, domain)) {}

  driver_session_guard(const driver_session_guard&) = delete;

  driver_session_guard& operator=(const driver_session_guard&) = delete;

  ~driver_session_guard() {
    current_driver_domain = previous_;  // 所有返回路径都撤销本次线程内 session。
  }

 private:
  io_domain* previous_;  ///< 仅借用地址；真正域租约由 driver/caller 生命周期持有。
};

/** @brief 统一操作/资源状态机；核心仅发布函数表结果，不直接恢复协程。
 * @details submit/cancel/close 可以跨线程调用；同一驱动 session 只有一个
 * poller。原生后端直接提交稳定 typed 请求、由完成包完成；仅无原生异步
 * API

 * * 的文件控制、目录与 DNS
 * 在专属执行器运行，普通原生数据路径不进入线程服务。

 */
class io_domain : public std::enable_shared_from_this<io_domain> {
 public:
  explicit io_domain(engine_config config)
      : backend_(config.backend_factory ? config.backend_factory()
                 : config.reactor_factory
                     ? backend_box{std::make_unique<readiness_adapter>(config.reactor_factory())}
                     : make_platform_backend(config.native_queue_entries)),
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
    // 全部操作槽在初始化阶段预热，普通 read/write 热路径不向通用堆申请内存。
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

  bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

  std::stop_token stop_token() const noexcept { return stop_.get_token(); }

  execution::blocking_executor_ref blocking() const noexcept {
    return execution::blocking_executor_ref{fs_};
  }

  execution::blocking_executor_ref cleanup() const noexcept {
    return execution::blocking_executor_ref{cleanup_};
  }

  execution::blocking_executor_ref resolver() const noexcept {
    return execution::blocking_executor_ref{dns_};
  }

  io_capabilities capabilities() const noexcept {
    io_capabilities c;
    c.backend = backend_.name();
    c.native_filesystem = backend_.native() && supports_native(operation_kind::read)
                          && supports_native(operation_kind::write);
    c.zero_copy = supports_native(operation_kind::send_zc);
    c.native_accept_nowait = supports_native(operation_kind::accept_nowait);
    return c;
  }

  bool supports_native(operation_kind kind) const noexcept {
    return backend_.native() && backend_.supports(static_cast<std::uint32_t>(kind));
  }

  backend_statistics statistics() const noexcept { return backend_.statistics(); }

  void wake() const noexcept { backend_.wake(); }

  /** @brief 外层 File 租约排空后注销原生缓存，关闭前防止数值重用命中旧注册。 */
  void forget_native_handle(native_descriptor handle, native_handle_kind kind) noexcept {
    std::lock_guard lock(mutex_);
    backend_.detach({static_cast<std::uintptr_t>(handle), kind});
  }

  io_context balanced_context() noexcept {
    if (placement_)
      return io_context{placement_->select()};
    return stopped() ? io_context{} : io_context{shared_from_this()};
  }

  /** @brief 只读取本域已经登记的 fd 身份；查找不会执行系统调用或借用注册。
   * @details 弱引用提升与 handle 核对在域锁内完成，返回的拥有型租约固定原资源。
   */
  resource_ptr find_registered_resource(native_descriptor fd) noexcept {
    std::lock_guard lock(mutex_);              // 与注册、关闭及描述符表更新使用相同短锁。
    const auto found = descriptors_.find(fd);  // 未登记时不得创建另一份资源身份。
    if (found == descriptors_.end())
      return {};
    auto resource = found->second.lock();  // 操作准备前先保留稳定资源和 owner 的租约。
    return resource && resource->fd() == fd ? std::move(resource) : resource_ptr{};
  }

  /** @brief raw fd 冷路径复用已有归属；带 resource 的正常 IO 不调用此接口。
   * @param include_runtime 是否允许查询本 runtime 的其他 shard；显式 context
   * 只查本域。
   * @details 本域锁释放后才读取弱 shard 表，任何时候都不同时持有两种控制面锁。
   */
  resource_ptr find_raw_resource(native_descriptor fd, bool include_runtime) noexcept {
    if (auto resource = find_registered_resource(fd))
      return resource;  // 已登记本域身份优先，不迁移资源或改变 fd 所有权。
    return include_runtime && placement_ ? placement_->find_resource(fd, this) : resource_ptr{};
  }

  /** @brief 注册长期资源；不接管失败的 fd，所有权仍归调用方。 */
  resource_ptr adopt(native_descriptor fd, bool owns = true, bool regular = false) {
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
    r->id = ++next_resource_;  // key 永不复用；零 key 预留为唤醒通道。
    if (!r->id)
      throw std::overflow_error("IO resource token exhausted");
    r->handle.store(fd, std::memory_order_release);
    r->regular_file = regular;
    r->native_kind =
        regular ? native_handle_kind::windows_handle : native_handle_kind::windows_socket;
    if (const int error = backend_.attach({static_cast<std::uintptr_t>(fd), r->native_kind}, r->id);
        error)
      throw std::system_error(-error, std::generic_category(), "IOCP attach");
    r->registered = true;
    r->socket_family =
        query_socket_family(fd);  // fd family 与绑定归属一起固定，native 扩展不能盲用于 Unix/pipe。
    resources_.emplace(r->id, r);
    descriptors_[fd] = r;
    r->owns_handle = owns;  // 只有所有注册成功后才接管 fd。
    return r;
  }

  /** @brief 借用 fd 的兼容注册；长期高性能路径应传 resource_ptr。 */
  /** @brief 借用完整位宽 SOCKET；IOCP 不接管调用方的关闭责任。 */
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

  /** @brief 原生导入对象首次挂起时绑定归属，保留原共享控制块。 */
  void bind(const resource_ptr& resource) {
    std::lock_guard binding_lock(
        resource->binding_mutex);  // 首次归属在资源局部串行，跨 domain 安全。
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
    if (const int error = backend_.attach(
            {static_cast<std::uintptr_t>(resource->fd()), resource->native_kind}, id);
        error) {
      resources_.erase(id);
      descriptors_.erase(resource->fd());
      throw std::system_error(-error, std::generic_category(), "IOCP attach");
    }
    resource->registered = true;
    resource->socket_family = query_socket_family(resource->fd());
    resource->owner = shared_from_this();
  }

  expected<void> reserve(resource_state& resource, Interest interest, void* owner) noexcept {
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

  void unreserve(resource_state& resource, Interest interest, void* owner) noexcept {
    std::lock_guard lock(mutex_);
    const auto bits = static_cast<unsigned>(interest);
    if (bits & 1 && resource.read_reserved == owner)
      resource.read_reserved = nullptr;
    if (bits & 2 && resource.write_reserved == owner) {
      resource.write_reserved = nullptr;
      // 组合写操作保留整段写方向；最后租约退出才允许此前请求的半关闭。
      if (resource.write_shutdown && !resource.writer && resource.fd() >= 0)
        issue_shutdown_write(resource);
    }
  }

  /** @brief 最后控制块退出；注销和 fd 接管都在同一个串行状态协议中完成。 */
  void release_resource(resource_state& resource) noexcept {
    std::lock_guard lock(mutex_);
    const native_descriptor fd =
        resource.handle.exchange(-1, std::memory_order_acq_rel);  // 唯一接管原生关闭责任。
    if (resource.registered && fd >= 0)
      backend_.detach({static_cast<std::uintptr_t>(fd), resource.native_kind});
    resources_.erase(resource.id);
    if (fd >= 0)
      descriptors_.erase(fd);
    if (resource.owns_handle && fd >= 0) {
      if (defer_native_close(fd))
        return;  // 原生析构直接接管关闭控制责任，不启动cleanup线程。
      if (needs_deferred_close(resource, fd))
        defer_cleanup([fd, kind = resource.native_kind] { (void)close_native(fd, kind); });
      else
        (void)close_native(fd, resource.native_kind);  // 关闭责任仅执行一次。
    }
  }

  /** @brief 仅在无在途请求时导出所有权，所有 split 包装同时观察 detached。 */
  expected<native_descriptor> detach(resource_state& resource,
                                     bool preserve_association = false) noexcept {
    std::lock_guard lock(mutex_);
    if (resource.active || resource.close_waiter || resource.read_reserved
        || resource.write_reserved)
      return std::unexpected{make_error(EBUSY)};
    const native_descriptor fd =
        resource.handle.exchange(-1, std::memory_order_acq_rel);  // 唯一接管原生关闭责任。
    if (fd < 0)
      return std::unexpected{make_error(EBADF)};
    if (resource.registered && !preserve_association)
      backend_.detach({static_cast<std::uintptr_t>(fd), resource.native_kind});
    resource.registered = false;
    resource.closing = true;  // 先发布 closing，后续 syscall 无法跨关闭边界取得执行权。
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
    return try_immediate_impl(request, already_stopped);  // 原完整请求入口保留。
  }

  /** @brief 紧凑 RECV/SEND 与完整请求共用资源锁、停止、deadline 和方向校验。 */
  std::optional<immediate_result> try_immediate(scalar_io_request& request,
                                                bool already_stopped) noexcept {
    return try_immediate_impl(request, already_stopped);
  }

 private:
  // 两个明确的请求策略共用同一状态机，避免紧凑路径遗漏取消/关闭/方向规则。
  template <class Request>
  std::optional<immediate_result> try_immediate_impl(Request& request,
                                                     bool already_stopped) noexcept {
    if (backend_.native())
      return {};  // 原生 Proactor 操作全部经 原生提交/完成端口，不能用同步
    // syscall 冒充 native 完成。
    if constexpr (std::is_same_v<Request, scalar_io_request>) {
      if (request.kind != operation_kind::recv && request.kind != operation_kind::send)
        return {};  // 紧凑策略没有其他 opcode 的输入，不能将其解释为
                    // read/write。
    } else if (request.kind != operation_kind::recv && request.kind != operation_kind::send
               && request.kind != operation_kind::read && request.kind != operation_kind::write) {
      return {};
    }
    const auto& resource =
        request.resource;  // awaiter 的请求租约已覆盖整个瞬时调用，无需重复计数。
    if (!resource || resource->regular_file)
      return {};  // raw fd 尚未借用以及阻塞文件沿用稳定槽协议。
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
      return immediate_result{-EINVAL, 0};  // 非法身份不执行 IO 或占用方向。
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
      // 与执行权校验/真实 syscall 同一线性化点；EAGAIN 后 prepare
      // 失败也由外层租约释放。
      request.establish_reservation = false;
    }
    request.fd = resource->fd();  // syscall 期间 close 无法取得此短锁，fd 不会被复用。
    const auto result = [&] {
      if constexpr (std::is_same_v<Request, scalar_io_request>) {
        return perform_socket_scalar(request);  // 仅消费精确定义的 scalar 输入。
      } else {
        bool connecting = false;
        return perform(request,
                       connecting);  // 通用 read/write 等协议保持原入口。
      }
    }();
    if (result == -EAGAIN || result == -EWOULDBLOCK) {
      resource->readiness &= ~direction;
      request.observed_would_block = true;
      request.observed_readiness_generation = resource->readiness_generation;
      return {};  // 释放瞬时路径锁后走正常 prepare+submit；不存在悬挂的 buffer
                  // 引用。
    }
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
    std::lock_guard lock(mutex_);  // 独立 prepare 的公开协议与 Prepared 取消窗口保持不变。
    return prepare_locked(std::move(request), target, error);
  }

  /** @brief 左值请求兼容入口：只复制一次拥有参数，再进入同一稳定槽准备协议。
   * @details 常规 awaiter
   * 使用右值重载而不建立中间请求；此入口保留原左值的所有权。 路径/iovec
   * 分配失败和原按值调用一样发生在准备以前，向调用方传播异常。
   */
  operation_token prepare(const io_request& request, completion_target target, int& error) {
    io_request owned = request;  // 描述符与路径拥有型复制；不提前注册资源或提交 原生请求。
    return prepare(std::move(owned), target, error);
  }

  /** @brief 接受槽位并推进一次非阻塞 attempt；终态由统一完成链交付。
   * @param token prepare 返回的 token；旧代际及重复提交均无副作用。
   * @details 所有资源 gate 在同一个短锁内取得；文件请求交给隔离执行器。
   *          同步完成也经发布协议，协程桥根据 gate 返回“不挂起”。
   */
  void submit(operation_token token) noexcept {
    bool wake_needed = false;
    bool native_completion_opportunity = false;
    {
      std::lock_guard lock(mutex_);  // 独立 submit 仍只接受一次，重复/旧代际调用无副作用。
      if (!submit_locked(token, wake_needed, native_completion_opportunity))
        return;
    }
    finish_submission(token, wake_needed,
                      native_completion_opportunity);  // 域锁外交付与通知。
  }

  /** @brief 协程桥在一个域锁内准备并接受 stable request，省去中间一次互斥往返。
   * @param stop 只在本调用内观察停止状态；不会保存 token 的借用地址或注册回调。
   * @param cancellable false 保持原桥不订阅任务停止的策略，已接管 CLOSE 仍看
   * uncancellable。
   * @details 槽选取/代际/方向 gate/原生 原生请求 全部复用独立 prepare/submit
   * 的同一状态机。 Prepared 后、accepted 前检查 sticky cancel；发布/driver/wake
   * 全部在域锁外。 target 可在返回以前立即完成，调用方须像原桥一样先建立 arming
   * gate 与拥有租约。
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
      std::lock_guard lock(mutex_);  // prepare → sticky cancel → accept 共用唯一域锁。
      token = prepare_locked(std::move(request), target, error);
      if (!token.value)
        return {};  // 尚未取得槽，不提交 原生请求、不保存
      // consumer，也不发布虚假完成。
      if (cancellable && stop.stop_requested()) {
        auto* operation = lookup(token);  // 新 Prepared 完整代际仍固定在本次域锁内。
        if (operation && !operation->request.uncancellable && !operation->cancellation)
          operation->cancellation = ECANCELED;  // 首次取消原因在 accepted 以前 sticky 保存。
      }
      if (!submit_locked(token, wake_needed, native_completion_opportunity))
        std::terminate();  // 本调用刚取得的 Prepared 槽不能被另一个接受者抢走。
    }
    finish_submission(token,
                      wake_needed,
                      native_completion_opportunity);  // 先释放域锁，保持原锁序。
    return token;                                      // gate 为 arming
    // 的消费者只观察结果，不能在此返回以前销毁调用帧。
  }

  /** @brief 取消只按代际 token 查槽，Prepared 阶段的取消同样 sticky 保留。 */
  void request_cancel(operation_token token, cancel_reason reason = cancel_reason::user) noexcept {
    {
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
      std::lock_guard lock(mutex_);
      begin_close(*resource, nullptr);
    }
    wake();
  }

  /** @brief 请求写半关闭；单次写和组合写方向租约全部排空后才发 SHUT_WR。 */
  void request_shutdown_write(resource_state& resource) noexcept {
    std::lock_guard lock(mutex_);
    resource.write_shutdown = true;
    if (!resource.writer && !resource.write_reserved && resource.fd() >= 0)
      issue_shutdown_write(resource);
  }

  /** @brief Windows 没有异步原生 close opcode；关闭责任由独立清理 lane 接管。
   */
  bool defer_native_close(native_descriptor, ::faio::move_only_function<void(int)> = {}) noexcept {
    return false;
  }

  /** @brief 公平有界驱动；批量内核事件、取消、deadline 和完成共用一套状态协议。
   */
  drive_result drive(drive_budget budget = {}, std::optional<int> wait = 0) noexcept {
    if (current_driver_domain)
      return {0, false, false, EBUSY};  // 同线程回调重入不再尝试已持有的 driver mutex。
    // 逻辑 session 禁止同时驱动，返回错误而非并发读取同一内核完成队列。
    std::unique_lock driver_lock(driver_mutex_, std::try_to_lock);
    if (!driver_lock.owns_lock())
      return {0, false, false, EBUSY};
    driver_session_guard session{this};  // 锁成功后才发布 session，退出前自动还原。
    std::array<backend_event, 256>
        events;  // 后端完整写入 count 个输出槽；不清零或读取未使用的整批空间。
    {
      std::lock_guard lock(mutex_);
      pump_native_pending();
      expire_deadlines();  // 先处理已到期请求，再计算本次内核等待上限。
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
    // poll 自己完成提交的后端只推进一次提交管线；其他后端保留独立 flush 合同。
    // 能力在后端类型创建时固定，不能仅按 native() 猜测第三方后端的 poll 行为。
    if (!backend_.poll_flushes_submissions()) {
      std::lock_guard lock(mutex_);  // 未自行提交的后端仍沿用 domain → backend 锁序。
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
      // 永久 reactor 错误仍交付每个 accepted 操作，不能遗留永不恢复的帧。
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
          continue;  // 控制 wake 不匹配普通资源。
        if (event.kind != backend_event_kind::readiness) {
          complete_native(event);
          continue;
        }
        auto it = resources_.find(event.key);  // key 是单调资源代际，fd 数值复用不会混淆。
        if (it == resources_.end())
          continue;  // fd 重用或 detach 后的迟到事件。
        auto resource = it->second.lock();
        if (!resource || resource->closing)
          continue;
        ++resource->readiness_generation;  // readiness guard
        // 只允许清除自己观察到的这一代。
        resource->readiness |= event.flags;  // 粘性提示仅用于唤醒，最终字节数/EOF 由 syscall 决定。
        if (resource->reader && event.flags & (readable_bit | error_bit))
          attempt(*resource->reader);
        if (resource->writer && event.flags & (writable_bit | error_bit))
          attempt(*resource->writer);
        // closed 是附加状态；后端已把 EOF/HUP 映射为真正受影响的方向位。
        auto* observer = resource->observers;
        while (observer) {
          auto* next = observer->observer_next;  // attempt 可能终结并摘除当前 observer。
          attempt(*observer);
          observer = next;
        }
      }
      expire_deadlines();  // 先处理已到期请求，再计算本次内核等待上限。
    }
    const auto published = publish_completed(std::max<std::size_t>(budget.max_completions, 1));
    bool more;
    {
      std::lock_guard lock(mutex_);
      pump_native_pending();
      more = completed_head_ != nullptr || !native_pending_.empty();
      if (free_.size() + retired_slots_ == operations_.size() && native_controls_.empty()
          && publishers_.load(std::memory_order_acquire) == 0 && backend_.quiescent())
        quiescent_cv_.notify_all();  // 最后 ACK 无业务完成，也要唤醒控制面 drain。
    }
    return {published, published != 0 || count > 0, more, fatal_error_};
  }

  std::optional<int> next_deadline() noexcept {
    std::lock_guard lock(mutex_);
    if (!next_deadline_)
      return {};
    const auto duration = std::chrono::ceil<std::chrono::milliseconds>(
                              *next_deadline_ - std::chrono::steady_clock::now())
                              .count();
    return static_cast<int>(std::clamp<std::int64_t>(duration, 0, INT_MAX));
  }

  bool quiescent() const noexcept {
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
    if (policy == shutdown_policy::cancel_all)
      stop_.request_stop();  // callback 不持有 domain
    // 状态锁，避免用户停止回调重入死锁。
    std::unordered_map<std::uint64_t, ::faio::move_only_function<void()>> cleanups;
    {
      std::lock_guard lock(mutex_);
      backend_.begin_shutdown();
      draining_ = policy == shutdown_policy::drain;  // drain 保留已接受请求，cancel_all 严格取消。
      cleanups.swap(shutdown_cleanups_);  // 回调移出锁外执行，可安全重新进入资源关闭接口。
      if (policy == shutdown_policy::cancel_all) {
        for (auto& entry : operations_)
          if (entry->allocated && !entry->terminal && !entry->request.uncancellable) {
            if (!entry->cancellation)
              entry->cancellation = ECANCELED;
            cancel_operation(*entry);
          }
      }
      // 保留 weak 表的迭代稳定性，close 在这里不擦除资源表。
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

  void shutdown() noexcept {
    begin_shutdown();
    // blocking syscall 不能强行终止，必须等真正返回后才释放借用 buffer。
    while (!quiescent())
      (void)drive({}, 10);
    if (own_fs_)
      fs_->close();
    if (own_dns_)
      dns_->close();
    if (own_cleanup_)
      cleanup_->close();
  }

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
      callback();  // 停机后的注册立即推进清理，不能遗漏刚完成的 open。
    return token;
  }

  void unregister_shutdown_cleanup(std::uint64_t token) noexcept {
    std::lock_guard lock(mutex_);
    shutdown_cleanups_.erase(token);  // 正常关闭及时回收注册项，反复 open 不会累积弱回调。
  }

  /** @brief 仅保存停机排空责任；等待外层lease退出时不创建线程/job/request。
   * @details File/ReadDir的唯一关闭owner取得票据，最终CLOSE/closedir后释放。
   */
  void retain_cleanup_wait() noexcept {
    if (publishers_.fetch_add(1, std::memory_order_acq_rel) == SIZE_MAX)
      std::terminate();  // 容量计数不得回绕为“已经排空”。
  }

  void release_cleanup_wait() noexcept {
    const auto previous = publishers_.fetch_sub(1, std::memory_order_acq_rel);
    if (!previous)
      std::terminate();  // 每张责任票据只能释放一次。
    if (previous == 1)
      quiescent_cv_.notify_all();
    wake();  // 真实关闭结束后驱动器可重新判定停机排空。
  }

  /** @brief 清理任务由专属 lane 执行；满载时保存在清理链而不是阻塞 worker。 */
  void defer_cleanup(::faio::move_only_function<void()> function) noexcept {
    // 完成责任用共享拥有型包装，try_submit 失败后仍能保留并重试任务。
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
        // 队列饱和仍保留任务；借用 buffer 清理必须由 shutdown 等待。
        std::lock_guard lock(mutex_);
        deferred_cleanup_.push_back(std::move(job));
        wake();
      }
    } catch (...) {
      std::terminate();
    }  // 无法保存关闭责任时不能静默泄漏或同步阻塞 IO worker。
  }

  std::pair<Ready, std::uint64_t> snapshot_readiness(
      const resource_state& resource) const noexcept {
    std::lock_guard lock(mutex_);
    return {Ready{resource.readiness, resource.readiness_generation},
            resource.readiness_generation};
  }

  void clear_readiness(resource_state& resource,
                       Interest interest,
                       std::uint64_t generation) noexcept {
    std::lock_guard lock(mutex_);
    if (resource.readiness_generation == generation)
      resource.readiness &= ~static_cast<std::uint32_t>(interest);
  }

  std::pair<Ready, std::uint64_t> snapshot_readiness(const resource_ptr& resource) const noexcept {
    std::lock_guard lock(mutex_);
    return {
        Ready{resource ? resource->readiness : 0, resource ? resource->readiness_generation : 0},
        resource ? resource->readiness_generation : 0};
  }

  /** @brief 只清除同一代际的观察结果，保留 guard 之后到达的新事件。 */
  void clear_readiness(const resource_ptr& resource,
                       Interest interest,
                       std::uint64_t generation) noexcept {
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
            || (result.error().domain() == error_domain::winsock
                && result.error().value() == WSAEWOULDBLOCK)))
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
    completion_target target{};    ///< callback 前复制；发布以后不再访问 consumer 地址。
    std::int64_t result{};         ///< 原最终 完成包/统一 finish 产生的业务结果。
    std::uint64_t transferred{};   ///< 统一状态机保存的实际进度。
  };

  /** @brief 调用者已持域锁的纯槽准备；不取得执行权、不提交 IO、不发布消费者。
   */
  operation_token prepare_locked(io_request&& request,
                                 completion_target target,
                                 int& error) noexcept {
    operation_state* selected = nullptr;
    // 退休槽不再计为可用容量，也不计为 in-flight；本次继续查找其他安全槽。
    while (!free_.empty()) {
      const auto candidate_index = free_.back();
      free_.pop_back();
      auto& candidate = *operations_[candidate_index];
      if (candidate.generation == UINT32_MAX) {
        ++retired_slots_;  // 永久退休，禁止 generation 回绕与迟到 token 碰撞。
        continue;
      }
      selected = &candidate;
      break;
    }
    if (!selected) {
      if (backend_.native()
          && (request.kind == operation_kind::close || request.internal_control)) {
        try {
          // 控制记录总数受资源/操作容量共同约束；不占用普通业务槽。
          if (native_controls_.size() >= max_resources_ + operations_.size()
              || next_control_ == UINT32_MAX)
            std::terminate();  // 关闭责任无法保存时不能泄漏或提前复用fd。
          const auto token = (static_cast<std::uint64_t>(++next_control_) << 32) | UINT32_MAX;
          auto owned = std::make_unique<operation_state>();
          owned->slot = UINT32_MAX;
          selected = native_controls_.emplace(token, std::move(owned)).first->second.get();
          selected->token = {token};
        } catch (...) {
          std::terminate();  // 无法拥有已接管fd时明确终止，禁止偷偷转线程仿真。
        }
      }
    }
    if (!selected) {
      error = EAGAIN;  // 所有未退休槽都在使用，调用方仍能在接受前安全回滚。
      if (free_.size() + retired_slots_ == operations_.size())
        quiescent_cv_.notify_all();
      return {};
    }
    auto& op = *selected;
    const auto index = op.slot;
    if (index != UINT32_MAX)
      ++op.generation;                // 控制记录已有不可复用token，普通槽仍使用代际协议。
    op.request = std::move(request);  // 直接从调用者移入稳定槽，免去按值形参的中间请求移动。
    op.target = target;               // 消费者仅由用户态统一发布链调用。
    if (index != UINT32_MAX)
      op.token = {static_cast<std::uint64_t>(op.generation) << 32 | (index + 1)};
    op.allocated = true;  // 从此开始占用容量，直到结果发布完毕才允许回收。
    op.accepted = false;  // Prepared 尚未执行 syscall，也没有资源 active 计数。
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
    op.cancellation = 0;  // 清除前一次代际的原因，新的取消只允许首次写入。
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
    auto* op = lookup(token);  // 必须同时检查 slot 与 generation，不能只凭地址找请求。
    if (!op || op->accepted)
      return false;
    op->accepted = true;  // 唯一接受边界；后续错误也必须向消费者交付一次结果。
    if (stopped() && op->request.kind != operation_kind::close && !op->request.uncancellable)
      finish(*op, -ECANCELED);
    else if (op->cancellation)
      finish(*op, -op->cancellation);
    else if (op->request.validation_error)
      finish(*op,
             -op->request.validation_error);  // 非法参数不取得 IO
                                              // 执行权，也不触发系统调用。
    else if (op->request.kind == operation_kind::ready
             && (op->request.argument <= 0 || op->request.argument > 3))
      finish(*op, -EINVAL);  // 原生就绪观察 与 readiness adapter 使用同一
                             // Interest 契约。
    else if (op->request.kind == operation_kind::recvmsg && !op->request.output_message)
      finish(*op, -EFAULT);  // 在复制消息描述符前拒绝空输出指针。
    else if (op->request.kind == operation_kind::recvmsg
             && static_cast<std::uint64_t>(op->request.output_message->msg_iovlen)
                    > native_iov_limit)
      finish(*op, -EINVAL);
    else if (op->request.kind == operation_kind::recvmsg && op->request.output_message->msg_iovlen
             && !op->request.output_message->msg_iov)
      finish(*op, -EFAULT);
    else if (op->request.kind == operation_kind::close && !op->request.resource
             && op->request.fd < 0)
      finish(*op, -EBADF);  // 原始整数 close 不具有可幂等查询的共享身份。
    else {
      try {
        auto& req = op->request;
        if (!req.bypass_resource_registration && !req.resource && req.fd >= 0
            && req.kind != operation_kind::open && req.kind != operation_kind::open2
            && req.kind != operation_kind::socket)
          req.resource = borrow(req.fd);
        // 当前 allocated 槽拥有资源 lease；下述关闭复用只改字段，不替换
        // resource。
        const auto& r = req.resource;
        if (r && r->owner.get() != this)
          finish(*op, -EXDEV);
        else if (r && req.kind != operation_kind::close && (r->fd() < 0 || r->closing))
          finish(*op, -EBADF);
        else if (req.establish_reservation && !r)
          finish(*op,
                 -EBADF);  // 组合租约只能归属于稳定资源，不能凭 raw fd 建立。
        else {
          if (r)
            req.fd = r->fd();
          if (r)
            req.native_kind = r->native_kind;
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
                finish(*op, -EINVAL);  // 取消/validation/deadline
                                       // 均已先仲裁，不留下空租约。
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
                  req.establish_reservation =
                      false;  // 原生 完成包/后续短写之间仍由组合帧保持排他。
                }
                if (direction & 1) {
                  r->reader = op;
                  op->uses_reader = true;
                }
                if (direction & 2) {
                  r->writer = op;
                  op->uses_writer = true;
                }
                ++r->active;  // 覆盖排队、syscall 及严格取消排空；close
                // 必须等待归零。
                op->counts_active = true;  // finish 仅为实际取得的资源租约减一次计数。
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
              else if (is_file_request(req))
                submit_file(*op);
              else if (req.observed_would_block && r && r->registered
                       && req.observed_readiness_generation == r->readiness_generation
                       && !(r->readiness & (direction_of(req.kind) | error_bit))) {
                // 同一已注册 ET 资源、没有更新事件：真实 EAGAIN
                // 已执行，无需再次 syscall。 新边沿尚在内核则下一 drive
                // 会处理；已消费的更新边沿必改变 generation。closed 仅是信息，
                // 对应 EOF/HUP 必须带本方向位，不能以相反方向半关闭强制重复
                // attempt。
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
      // 外域提交必须通知实际 owner，不能凭本请求终态省略通知。
      const auto* pending = lookup(token);  // 域锁内检查完整代际；此处尚未开始锁外发布。
      // 本 token 之外的完成仍由 owner driver 发布；typed Cancel
      // 可先终结原请求再终结自己。 head 是本 token 时，next
      // 非空即证明还有另一槽；否则非空 head 自身就是另一 token。
      const bool other_completion =
          completed_head_ && (completed_head_->token.value != token.value || completed_head_->next);
      if ((pending && !pending->terminal) || other_completion)
        wake_needed = true;  // 锁外 publish_one 只领取本 token，随后唤醒以交付其他完成。
    }
    // 复用 submit 已持有的域锁作 O(1) 资格快照，免去机会函数的额外首轮域锁。
    // 快照不接管 CQ 责任；取消/远程提交改变状态时，driver
    // 获锁后的二次校验仍拒绝。
    native_completion_opportunity = current_domain == this && !current_driver_domain
                                    && backend_.native() && can_complete_local_native(token);
    return true;
  }

  /** @brief 接受以后锁外处理低负载 native 机会、即时交付与远程控制通知。
   * @details 仅重用既有 原生提交/完成
   * 和发布协议；不创建子协程、辅助队列或工作线程。
   */
  void finish_submission(operation_token token,
                         bool wake_needed,
                         bool native_completion_opportunity) noexcept {
    // 本地机会沿 driver → domain → backend 原锁序，只尝试一次真实 完成包。
    // 当前 token 若终结，在 complete_native
    // 同一个域锁内领取，免去再次锁定领取。
    const auto delivery =
        native_completion_opportunity ? try_complete_local_native(token) : claimed_completion{};
    // 成功机会函数已经退出它自己的 driver/session/domain
    // 全部作用域，再发布消费者。
    if (delivery.operation)
      publish_claimed(delivery);  // 只发布本 token；其余完成仍留在原 owner 完成链。
    else
      publish_one(token);  // miss/锁失败/远程/readiness 保持原领取与竞态补查路径。
    if (wake_needed)
      wake();  // 远程新deadline/等待者登记打断无限poll，本地快路径不做唤醒syscall。
  }

  /** @brief 只在首次注册读取 socket family；普通文件/pipe 返回 AF_UNSPEC。 */
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

  /** @brief 写方向与组合租约全部排空后只执行一次 Winsock 半关闭。 */
  void issue_shutdown_write(resource_state& resource) noexcept {
    if (resource.write_shutdown_issued || resource.closing || resource.fd() < 0)
      return;
    resource.write_shutdown_issued = true;  // 和写方向 admission 在同一个域锁下仲裁。
    (void)::shutdown(static_cast<SOCKET>(resource.fd()),
                     SD_SEND);  // 不关闭读方向。
  }

  /** @brief native 能力固定，零拷贝接口使用等价 OVERLAPPED 发送而不宣称零拷贝。
   */
  bool can_native_request(const io_request& request) const noexcept {
    if (request.native_kind == native_handle_kind::windows_handle && request.windows_implicit_cursor
        && ::GetFileType(reinterpret_cast<HANDLE>(request.fd)) == FILE_TYPE_DISK)
      return false;  // raw 默认游标必须序列化读取/更新 OS 指针；File 的定位 IO
    // 不走此分支。
    return request.kind == operation_kind::send_zc || request.kind == operation_kind::sendmsg_zc
           || supports_native(request.kind);
  }

  /** @brief 域锁内判断低负载原生机会，不扫描操作槽或增加热路径计数器。
   * @details listener ACCEPT 与一条连接可共用域；至多八个已分配槽和资源才触发。
   *          资源数门控保守保留多连接批处理，已有完成/控制/背压优先交给正常
   * driver。
   */
  bool can_complete_local_native(operation_token token) noexcept {
    const auto allocated = operations_.size() - free_.size() - retired_slots_;
    if (allocated > 8 || resources_.size() > 8 || !native_controls_.empty()
        || !native_pending_.empty() || completed_head_ || fatal_error_ || stopped())
      return false;                   // 只使用已有容器大小，忙域和排空阶段不拆散提交批次。
    auto* operation = lookup(token);  // 重新核对完整代际，不复用已发布或回收的槽地址。
    return operation && operation->accepted && operation->native_inflight && !operation->terminal
           && !operation->request.internal_control && operation->target.publish;
    // 内置 SHUTDOWN/孤立 CLOSE 的 submit 可能仍在外层域锁内，不能尝试取得
    // driver 锁。 只允许有业务消费者的普通原生请求；内部控制沿原批量 driver
    // 管线排空。
  }

  /** @brief 本地低负载的一次原生提交/完成机会；不等待，不执行同步业务 syscall。
   * @param token submit 当前操作的完整代际，完成后仍由其原 publish_one 交付。
   * @details driver mutex 只 try_lock，未取得或不再符合条件即保持原挂起行为。
   *          poll(0) 内部已经 flush；禁止另做重复 flush 或循环等待 完成包。
   *          ACK/MORE/NOTIF 全部复用 complete_native 的租约与唯一终态协议。
   */
  claimed_completion try_complete_local_native(operation_token token) noexcept {
    if (current_domain != this || current_driver_domain || !backend_.native())
      return {};  // 远程请求、已有 CQ session 和 readiness
    // 后端均保留原驱动方式。
    // 首次资格已在 submit 的原有域锁内快照；这里不重复取得域锁或等待 CQ
    // 消费者。
    std::unique_lock driver_lock(driver_mutex_, std::try_to_lock);
    if (!driver_lock.owns_lock())
      return {};                         // 已有唯一消费者时不等待、不争抢其 CQ 完成责任。
    driver_session_guard session{this};  // 内部 close/callback 再 submit 时跳过机会。
    int failure{};
    claimed_completion delivery;  // 仅拥有领取责任的标量快照；不复制资源 lease。
    {
      std::lock_guard lock(mutex_);  // 锁序与正常 drive 一致：driver → domain → backend。
      if (!can_complete_local_native(token))
        return {};  // 两锁之间可能发生取消/远程提交，必须再次核对资格和代际。
      std::array<backend_event, 8> events;  // 单批真实 完成包 输出；只读取返回数量内的完整记录。
      const int count =
          backend_.poll(events, 0);  // 一次非阻塞真实完成包消费；IOCP flush 是空操作。
      if (count < 0)
        failure = -count;  // 永久故障仍由原 drain/fail-fast 协议处理，不提前释放
      // buffer。
      else {
        // 所有真实结果进入同一状态机；ACK/MORE/NOTIF
        // 保留原唯一终态和最终释放协议。
        for (int index = 0; index < count; ++index)
          complete_native(events[static_cast<std::size_t>(index)]);
        // 完成与领取共用原域锁；只摘本 token，稳定 request
        // 仍由该槽拥有到发布返回。
        delivery = claim_completed_locked(token);
      }
    }
    if (failure)
      enter_failure(failure);  // 域锁外处理停机回调；session 仍阻止驱动重入。
    // return 销毁 session 和 driver_lock 以后，finish_submission 才能执行未知
    // callback。
    return delivery;
  }

  /** @brief 永久后端故障先建立内核 drain 边界，不能把仍在途的 buffer
   * 当作失败结果发布。 */
  void enter_failure(int error) noexcept {
    {
      std::lock_guard lock(mutex_);
      if (fatal_error_)
        return;
      fatal_error_ = error;
      if (!backend_.begin_failure(error))
        std::terminate();  // 不可证明 kernel release 时明确终止，拒绝静默悬挂/
      // UAF。
      for (auto& entry : operations_)
        if (entry->allocated && entry->accepted && !entry->terminal
            && !entry->request.uncancellable)
          if (!entry->cancellation)
            entry->cancellation = error;
    }
    begin_shutdown();  // 锁外发 stop 与外部 cleanup
                       // 回调，资源关闭仍等待原生最后引用。
  }

  /** @brief native accepted 与内核引用分离；稳定池满不会丢请求，也不在 worker
   * 阻塞。
   * @details pending 链最多操作池容量；请求始终在槽内，路径、iovec
   * 描述符地址稳定。
   */
  void submit_native(operation_state& op) noexcept {
    // 显式流式空请求不借用内核 buffer；datagram 不能套用这个成功规则。
    if (empty_stream_request(op.request)) {
      finish(op, 0);
      return;
    }
    // 请求已经在稳定槽内，后端接受后即承担 native lease，原 API
    // 已经提交，payload 不能提前回收。
    const auto result =
        fatal_error_ ? backend_submit_result{backend_submit_status::rejected, fatal_error_}
                     : backend_.try_submit(
                           {op.token.value, &op.request});  // 接受后保留请求借用到原生最终完成。
    if (result.status == backend_submit_status::accepted) {
      op.native_inflight = true;  // 最终原请求 完成包 或 buffer release 前禁止 finish。
      // 已经转交后端 的请求不再占域 pending 队列责任。
      op.native_pending = false;
    } else if (result.status == backend_submit_status::would_queue
               || ((op.request.kind == operation_kind::close || op.request.internal_control)
                   && result.error == ENOMEM)) {
      // 只登记一次背压责任；普通请求 稳定池满与关闭控制接受前 ENOMEM
      // 都可保留重试。
      if (!op.native_pending) {
        op.native_pending = true;
        try {
          // 保存完整代际而非槽指针，回收后迟到 pending 项不会命中新操作。
          native_pending_.push_back(op.token);
        } catch (...) {
          op.native_pending = false;
          if (op.request.kind == operation_kind::close || op.request.internal_control)
            std::terminate();  // 控制责任不能以ENOMEM结果提前归还尚未关闭fd。
          // 普通请求尚未被 backend 接受，才允许分配失败形成安全终态。
          finish(op, -ENOMEM);
        }
      }
    } else {
      op.native_pending = false;
      if (op.request.kind == operation_kind::close) {
        // ENOMEM/稳定池背压已保留重试；其余接受前永久错误无法合法丢弃关闭责任。
        std::terminate();
      } else
        // 原生接受前拒绝没有 kernel lease，负 errno 仍经统一终态发布。
        finish(op, -result.error);
    }
  }

  /** @brief 一批有界重试；完成包消费后释放稳定节点容量，再把排队请求交给后端。
   */
  void pump_native_pending() noexcept {
    // 每轮最多 256 次重试，不因 稳定池持续满载而在 worker 忙循环。
    const auto limit = std::min<std::size_t>(native_pending_.size(), 256);
    for (std::size_t i = 0; i < limit; ++i) {
      // 先取稳定代际，再弹出旧责任；重新背压时 submit_native 会放回队列。
      const auto token = native_pending_.front();
      native_pending_.pop_front();
      auto* op = lookup(token);
      // 已消费/取消或重复旧项只丢弃数值记录，不访问已复用请求。
      if (!op || op->terminal || !op->native_pending)
        continue;
      // 当前项暂由本轮负责，下一次 would_queue 才重新登记。
      op->native_pending = false;
      // 未交内核的普通取消可以直接完成，已接管关闭责任不能被取消跳过。
      if (op->cancellation && !op->native_close && !op->request.uncancellable)
        finish(*op, -op->cancellation);
      else
        submit_native(*op);
    }
  }

  /** @brief 取消 ACK 不解除 payload 租约；这里只要求内核取消，原 完成包
   * 决定终态。
   */
  void cancel_operation(operation_state& op) noexcept {
    if (op.native_close || op.request.uncancellable)
      return;  // 唯一 close 责任只允许真实完成。
    if (op.native_inflight) {
      if (!op.native_cancel_requested && !op.native_close) {
        // 一次发布取消责任，原请求仍保持 native_inflight 直到最后 完成包。
        op.native_cancel_requested = true;
        // 精确 token + 稳定 request 交后端，不按数字 fd 取消可能重用的资源。
        backend_.request_cancel({op.token.value, &op.request});
      }
    } else if (!op.running_file) {
      // 尚无内核/文件服务引用才可撤销待提交责任，running_file 必须等真实返回。
      op.native_pending = false;
      finish(op, -op.cancellation);
    }
  }

  /** @brief 原生结果与资源就绪在核心汇合；MORE 结果保留到 NOTIF 后才发布一次。
   */
  void complete_native(const backend_event& event) noexcept {
    if (event.kind == backend_event_kind::cancel_ack)
      return;  // ACK 可早于原请求完成，不能回收 generation 或恢复用户。
    // 后端整数事件重新核对域 slot/generation，迟到业务 完成包 不能访问复用槽。
    auto* op = lookup(operation_token{event.key});
    if (!op || !op->native_inflight || op->terminal)
      return;
    if (event.kind == backend_event_kind::buffer_release) {
      if (!op->native_waiting_release)
        return;
      // 最终释放通知解除 payload lease；不能把 NOTIF 的 res 当成发送结果。
      op->native_waiting_release = false;
      op->native_inflight = false;
      finish(*op,
             op->result);  // NOTIF 没有业务字节数，使用前一个发送 完成包 的结果。
      return;
    }
    // 保留真正业务 完成包 结果，后续取消仲裁只改变错误，不抹去已完成进度。
    op->result = event.result;
    // 中立 MORE 表示还有内核引用，结果先保存，绝不提前恢复借用数据的协程。
    if (event.flags & 1u) {
      op->native_waiting_release = true;
      return;
    }
    // 最终业务 完成包 已解除 native lease，统一 finish 才能登记唯一终态。
    op->native_inflight = false;
    if (op->native_close) {
      // CLOSE 的资源租约仍在本槽，所有 close waiter 只能在真实关闭结果后完成。
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
      std::uint32_t flags{};
      if (event.result & (POLLIN | POLLPRI))
        flags |= readable_bit;
      if (event.result & POLLOUT)
        flags |= writable_bit;
      if (event.result & (POLLERR | POLLNVAL))
        flags |= error_bit;
      if (event.result & POLLHUP)
        flags |=
            readable_bit | writable_bit | closed_bit;  // 原生全 HUP 与 reactor 保持双方向终态提示。
#ifdef POLLRDHUP
      if (event.result & POLLRDHUP)
        flags |= readable_bit | read_closed_bit;  // 原生读半关闭不能冒充写方向关闭。
#endif
      // 只有显式原生 ready 请求在此生成观察代际，数据 IO 不转换成 readiness。
      ++resource.readiness_generation;
      // 保留方向和关闭信息；真实 EAGAIN 或同代际 guard 再清对应方向。
      resource.readiness |= flags;
      finish(*op, flags);
    } else {
      finish(*op, event.result);
    }
  }

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

  static std::uint32_t direction_of(operation_kind kind) noexcept {
    switch (kind) {
      case operation_kind::recv:
      case operation_kind::recvfrom:
      case operation_kind::recvmsg:
      case operation_kind::accept:
      case operation_kind::accept_nowait:
      case operation_kind::read:
      case operation_kind::readv:
        return 1;
      case operation_kind::send:
      case operation_kind::sendto:
      case operation_kind::sendmsg:
      case operation_kind::send_zc:
      case operation_kind::sendmsg_zc:
      case operation_kind::connect:
      case operation_kind::write:
      case operation_kind::writev:
        return 2;
      default:
        return 0;
    }
  }

  static bool is_file_request(const io_request& req) noexcept {
    return req.kind == operation_kind::open || req.kind == operation_kind::open2
           || req.kind == operation_kind::fsync
           || ((req.kind == operation_kind::read || req.kind == operation_kind::write
                || req.kind == operation_kind::readv || req.kind == operation_kind::writev)
               && req.native_kind == native_handle_kind::windows_handle)
           || (req.kind == operation_kind::close
               && req.native_kind == native_handle_kind::windows_handle);
  }

  /** @brief 检查 close 是否可能等待磁盘或 SO_LINGER，阻塞关闭交给 cleanup
   * lane。
   * @details 普通文件必需异步关闭；socket 的正 linger 期限也不能占住 IO
   * worker。 pipe 等非 socket 的 getsockopt 返回 ENOTSOCK，只执行短的普通
   * close。
   */
  static bool needs_deferred_close(const resource_state& resource, native_descriptor fd) noexcept {
    if (resource.native_kind == native_handle_kind::windows_handle)
      return true;
    ::linger option{};
    int length = sizeof(option);
    return ::getsockopt(static_cast<SOCKET>(fd),
                        SOL_SOCKET,
                        SO_LINGER,
                        reinterpret_cast<char*>(&option),
                        &length)
               == 0
           && option.l_onoff && option.l_linger > 0;
  }

  /** @brief 只有操作租约排空后才执行一次对应种类的原生关闭器。 */
  static int close_native(native_descriptor fd, native_handle_kind kind) noexcept {
    if (kind == native_handle_kind::windows_handle)
      return ::CloseHandle(reinterpret_cast<HANDLE>(fd))
                 ? 0
                 : windows::encode_windows_error(::GetLastError());
    return ::closesocket(static_cast<SOCKET>(fd)) == 0
               ? 0
               : windows::encode_winsock_error(::WSAGetLastError());
  }

  /** @brief 已知字节流的空缓冲区返回成功；不对 UDP 或非空消息执行该优化。 */
  static bool empty_stream_request(const io_request& request) noexcept {
    if (!request.empty_success)
      return false;
    switch (request.kind) {
      case operation_kind::recv:
      case operation_kind::send:
      case operation_kind::send_zc:
      case operation_kind::read:
      case operation_kind::write:
        return request.length == 0;
      case operation_kind::readv:
      case operation_kind::writev:
      case operation_kind::sendmsg:
      case operation_kind::sendmsg_zc:
        return std::all_of(request.vectors.begin(), request.vectors.end(), [](const iovec& vector) {
          return vector.iov_len == 0;
        });
      case operation_kind::recvmsg:
        if (!request.output_message)
          return false;
        // 消息向量数量必须先转换到足够宽的类型再与有界上限比较。
        if (static_cast<std::uint64_t>(request.output_message->msg_iovlen) > native_iov_limit)
          return false;
        for (std::size_t i = 0; i < static_cast<std::size_t>(request.output_message->msg_iovlen);
             ++i)
          if (!request.output_message->msg_iov || request.output_message->msg_iov[i].iov_len != 0)
            return false;
        return true;
      default:
        return false;
    }
  }

  /** @brief IOCP 已实现的请求不经过此处；短控制与平台 fallback 在明确的 lane
   * 执行。 */
  template <class Request>
  static std::int64_t perform_socket_scalar(Request& r) noexcept {
    const int size = static_cast<int>(std::min<std::size_t>(r.length, INT_MAX));
    const int value =
        r.kind == operation_kind::recv
            ? ::recv(static_cast<SOCKET>(r.fd), static_cast<char*>(r.buffer), size, r.flags)
            : ::send(static_cast<SOCKET>(r.fd),
                     static_cast<const char*>(r.const_buffer),
                     size,
                     r.flags);
    return value == SOCKET_ERROR ? -windows::encode_winsock_error(::WSAGetLastError()) : value;
  }

  static std::int64_t perform(io_request& r, bool&) noexcept {
    const SOCKET socket = static_cast<SOCKET>(r.fd);
    int result{};
    switch (r.kind) {
      case operation_kind::shutdown:
        result = ::shutdown(socket, r.argument);
        break;
      case operation_kind::socket: {
        if (r.flags)
          return -EOPNOTSUPP;  // 原接口的flags是Linux提交扩展，不可静默忽略。
        const SOCKET created = ::WSASocketW(r.argument,
                                            r.argument2 & ~(SOCK_NONBLOCK | SOCK_CLOEXEC),
                                            r.argument3,
                                            nullptr,
                                            0,
                                            WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (created == INVALID_SOCKET)
          return -windows::encode_winsock_error(::WSAGetLastError());
        u_long mode = 1;
        if (::ioctlsocket(created, FIONBIO, &mode) == SOCKET_ERROR) {
          auto error = ::WSAGetLastError();
          ::closesocket(created);
          return -windows::encode_winsock_error(error);
        }
        return static_cast<std::int64_t>(created);
      }
      case operation_kind::getsockopt: {
        int length = static_cast<int>(r.length);
        result =
            ::getsockopt(socket, r.argument, r.argument2, static_cast<char*>(r.buffer), &length);
        if (!result)
          return length;
        break;
      }
      case operation_kind::setsockopt:
        result = ::setsockopt(socket,
                              r.argument,
                              r.argument2,
                              static_cast<const char*>(r.const_buffer),
                              static_cast<int>(r.length));
        break;
      case operation_kind::socket_inq: {
        u_long bytes{};
        result = ::ioctlsocket(socket, FIONREAD, &bytes);
        if (!result)
          return bytes;
        break;
      }
      case operation_kind::socket_outq:
        return -EOPNOTSUPP;
      case operation_kind::accept_nowait: {
        SOCKET accepted = ::accept(socket, r.output_address, r.output_address_length);
        if (accepted == INVALID_SOCKET) {
          auto e = ::WSAGetLastError();
          return e == WSAEWOULDBLOCK ? -EAGAIN : -windows::encode_winsock_error(e);
        }
        return static_cast<std::int64_t>(accepted);
      }
      case operation_kind::open:
        return windows::open_file(r);
      case operation_kind::read:
      case operation_kind::write:
        return windows::scalar_file(r);
      case operation_kind::readv:
      case operation_kind::writev:
        return windows::vectored_file(r);
      case operation_kind::close:
        return -close_native(r.fd, r.native_kind);
      case operation_kind::fsync:
        return ::FlushFileBuffers(reinterpret_cast<HANDLE>(r.fd))
                   ? 0
                   : -windows::encode_windows_error(::GetLastError());
      default:
        return -EOPNOTSUPP;
    }
    return result == SOCKET_ERROR ? -windows::encode_winsock_error(::WSAGetLastError()) : result;
  }

  /** @brief 一个资源只安装一次持久 readiness 注册，避免逐次 MOD/rearm 成本。 */
  int ensure_registration(resource_state& resource) noexcept {
    if (resource.registered)
      return 0;
    if (resource.regular_file)
      return -ENOTSUP;
    const int result = backend_.attach(
        {static_cast<std::uintptr_t>(resource.fd()), resource.native_kind}, resource.id);
    if (!result)
      resource.registered = true;
    return result;
  }

  /** @brief readiness adapter 先尝试真实 IO，仅 EAGAIN 才保留挂起状态。 */
  void attempt(operation_state& op) noexcept {
    if (op.terminal || op.running_file
        || op.native_inflight)  // 文件 job 拥有请求，driver 不得同时触碰其缓冲区。
      return;
    // 嵌套 finish 只登记完成链，当前 stable 槽在本函数返回以前不会回收。
    const auto& resource = op.request.resource;
    if (op.cancellation) {
      finish(op, -op.cancellation);
      return;
    }
    if (op.request.kind == operation_kind::ready) {
      const auto mask = static_cast<std::uint32_t>(op.request.argument);
      if (!mask || mask > 3 || !resource) {
        finish(op, -EINVAL);
        return;
      }
      if (const int e = ensure_registration(*resource); e) {
        finish(op, e);
        return;
      }
      // 半关闭附加信息不能替代 Interest；对端仍可在另一个方向发送响应。
      if (resource->readiness & (mask | error_bit))
        finish(op, resource->readiness);
      return;
    }
    if (op.request.kind == operation_kind::cancel) {
      if (static_cast<unsigned>(op.request.flags) & ~3u) {
        finish(op, -ENOTSUP);  // ALL=1/FD=2 可等价；FD_FIXED/ANY
        // 需要额外身份，不能假装支持。
        return;
      }
      if (resource) {
        // 本资源最多一个reader/writer并可有多个observer/元数据；统一按代际请求逐一取消。
        // Cancel成功只表示控制请求已接受，借用buffer仍须等待各原请求的最终完成。
        for (auto& entry : operations_)
          if (entry.get() != &op && entry->allocated && entry->accepted && !entry->terminal
              && (entry->request.resource == resource
                  || (entry->request.bypass_resource_registration && !entry->request.resource
                      && entry->request.fd == op.request.fd))
              && entry->request.kind != operation_kind::close && !entry->request.uncancellable) {
            // File 原生请求由外层 active lease 固定 fd；只匹配本域明确 bypass
            // 的借用请求。 普通资源仍比较共享身份，绝不凭相同整数 fd
            // 取消另一代已重用资源。
            if (!entry->cancellation)
              entry->cancellation = ECANCELED;
            cancel_operation(*entry);  // 取消控制与原请求
                                       // 完成包各自排空，不借控制成功提前恢复业务。
          }
      }
      finish(op, 0);
      return;
    }
    if (op.request.kind == operation_kind::sendmsg)
      op.request.message.msg_iov = op.request.vectors.data();
    const auto result = perform(op.request, op.connecting);
    if (result == -EAGAIN || result == -EWOULDBLOCK || result == -EINPROGRESS
        || result == -EALREADY) {
      if (!resource) {
        finish(op, result);
        return;
      }
      resource->readiness &= ~direction_of(op.request.kind);  // 真实 EAGAIN 才清除对应就绪提示。
      if (const int error = ensure_registration(*resource); error)
        finish(op, error);
      return;
    }
    finish(op, result);
  }

  /** @brief 有界文件 lane 接受 stable token，取消也要等 job
   * 真实结束才释放租约。 */
  void submit_file(operation_state& op) noexcept {
    op.running_file = true;          // 排队阶段也禁止 slot/buffer 回收，cancel 只记录 sticky 原因。
    auto self = shared_from_this();  // job 保留服务租约，停止后也不能访问已销毁的域。
    const auto token = op.token;     // 保存完整代际，不捕获会被下一代复用的裸槽地址。
    const auto submitted = fs_->try_submit([self, token] {
      operation_state* operation;
      {
        std::lock_guard lock(self->mutex_);  // job 真正开始与 cancel 使用同一串行边界。
        operation = self->lookup(token);     // accepted 租约覆盖排队、执行和完成，槽不能提前复用。
        if (operation->cancellation) {
          // 未开始 syscall 的取消不产生文件写入/open 副作用；close 由独立清理
          // lane 推进。
          operation->running_file = false;
          self->finish(*operation, -operation->cancellation);
          self->wake();
          return;
        }
      }
      bool connecting = false;
      if (operation->request.kind == operation_kind::close
          && operation->request.native_kind == native_handle_kind::windows_handle)
        self->forget_native_handle(operation->request.fd, native_handle_kind::windows_handle);
      const auto result = perform(operation->request,
                                  connecting);  // 释放状态锁后执行阻塞 syscall。
      {
        std::lock_guard lock(self->mutex_);
        operation->running_file = false;
        // 若 open 已创建 handle 但取消获胜，关闭副作用，不能泄漏新 fd。
        self->finish(*operation, result);
      }
      self->wake();
    });
    if (!submitted) {
      op.running_file = false;
      finish(op, -submitted.error().value());
    }
  }

  /** @brief terminal 只发布一次；阻塞 job 结束前不准进入 terminal。 */
  void finish(operation_state& op, std::int64_t result) noexcept {
    if (op.terminal || op.running_file
        || op.native_inflight)  // 文件 job 拥有请求，driver 不得同时触碰其缓冲区。
      return;
    if (op.cancellation && result >= 0
        && ((op.request.kind == operation_kind::accept
             || op.request.kind == operation_kind::accept_nowait)
            || op.request.kind == operation_kind::open || op.request.kind == operation_kind::open2
            || op.request.kind == operation_kind::socket)) {
      // 取消获胜但内核已创建 fd：先接管新 fd
      // 的唯一清理责任，不能泄漏或交用户复用。
      const native_descriptor fd = static_cast<native_descriptor>(result);
      if (!defer_native_close(fd))
        defer_cleanup([fd,
                       kind = op.request.kind == operation_kind::open
                                  ? native_handle_kind::windows_handle
                                  : native_handle_kind::windows_socket] {
          (void)close_native(fd, kind);
        });  // 取消后的新fd仍走原生CLOSE。
    }
    op.terminal = true;  // 唯一终态转换，从此不允许 syscall 重试或重复加入完成链。
    // 读写结果单位是字节，ACCEPT 的 fd 和 CONNECT 的状态绝不是传输量。
    op.transferred = result > 0 && direction_of(op.request.kind)
                             && op.request.kind != operation_kind::accept
                             && op.request.kind != operation_kind::accept_nowait
                             && op.request.kind != operation_kind::connect
                         ? static_cast<std::uint64_t>(result)
                         : 0;
    if (op.request.kind == operation_kind::unlinkat && result == 0)
      op.transferred = 1;  // 完成包成功已经删除一条；取消获胜也必须保留真实条目进度。
    if (op.request.kind == operation_kind::ready && result >= 0 && op.request.resource)
      // ready 结果的 progress 是观察代际，供 guard 清理同一批提示而非字节。
      op.transferred = op.request.resource->readiness_generation;
    op.result =
        op.cancellation ? -op.cancellation : result;  // 首次取消原因稳定，实际传输量另行保留。
    // 借用 stable 槽已有 lease；本函数不发布当前消费者，发布返回后才回收此槽。
    const auto& resource = op.request.resource;
    if (resource) {
      // 只解除本操作真正取得的方向 gate，不清其他代际的请求或 reservation。
      if (op.uses_reader && resource->reader == &op)
        resource->reader = nullptr;
      if (op.uses_writer && resource->writer == &op)
        resource->writer = nullptr;
      // 多观察者独立摘链，不消费业务数据或撤销其他 ready waiter。
      if (op.observer) {
        auto** link = &resource->observers;
        // 稳定槽地址只在域锁内作为链节点身份，内核和取消控制仍使用整数 token。
        while (*link && *link != &op)
          link = &(*link)->observer_next;
        // 找到本节点才重连后继，未登记的操作不改观察者链。
        if (*link)
          *link = op.observer_next;
      }
      // active 只对应已经登记方向/observer/普通资源请求，关闭 waiter 不占
      // active。
      if (op.counts_active)
        --resource->active;
      // 单次写完成不等于 write_all 完成：方向租约覆盖各 partial write 间隙。
      if (resource->write_shutdown && !resource->writer && !resource->write_reserved
          && resource->fd() >= 0)
        issue_shutdown_write(*resource);
    }
    op.completed_previous = completed_tail_;  // 双向节点允许 submit O(1) 领取自己的即时结果。
    // 完成链暂时接管发布责任，槽和 payload 参数仍保留到锁外 callback 返回。
    op.next = nullptr;
    // publish_one/publish_completed 用此标记领取一次责任，禁止重复交付结果。
    op.queued_completion = true;
    // O(1) 追加完成双向链，submit 可仅摘自己的即时终态而不发布远程完成。
    if (completed_tail_)
      completed_tail_->next = &op;
    else
      completed_head_ = &op;
    completed_tail_ = &op;  // 完成责任仍只有一次，结果消费者始终在锁外调用。
    // 最后 active 退出才接管真实关闭；closing 早已拒绝后续 IO。
    if (resource && resource->closing && !resource->active)
      close_handle(*resource);
    else if (resource && stopped() && draining_ && !resource->active)
      begin_close(*resource, nullptr);
  }

  /** @brief closing 的发布拒绝后续 IO，原先接受的请求被取消并严格排空。 */
  void begin_close(resource_state& resource, operation_state* waiter) noexcept {
    if (waiter) {
      waiter->request.uncancellable =
          true;  // 已接管的close waiter不能先回收，否则链会保留复用槽地址。
      waiter->observer_next = resource.close_waiter;
      resource.close_waiter = waiter;
    }
    resource.closing = true;  // 先发布 closing，后续 syscall 无法跨关闭边界取得执行权。
    // fsync 等无读写方向操作也必须参与关闭取消，不能只取消 reader/writer。
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
    while (observer) {
      auto* next = observer->observer_next;
      cancel_operation(*observer);
      observer = next;
    }
    if (!resource.active)
      close_handle(resource);
  }

  /** @brief 方向/操作租约排空后唯一接管 fd，文件与启用 linger 的 socket 经
   * cleanup lane 实际关闭后 才完成等待者。
   * @details handle.exchange(-1) 拒绝新 IO，但不代表内核 close 已完成；不能重复
   * close。
   */
  void close_handle(resource_state& resource) noexcept {
    if (resource.close_pending)
      return;
    const native_descriptor fd =
        resource.handle.exchange(-1, std::memory_order_acq_rel);  // 唯一接管原生关闭责任。
    if (fd >= 0) {
      if (resource.registered)
        backend_.detach({static_cast<std::uintptr_t>(fd), resource.native_kind});
      resource.registered = false;
      descriptors_.erase(fd);
      if (resource.owns_handle) {
        if (needs_deferred_close(resource, fd)) {
          // 文件或正 SO_LINGER 的 close 可能阻塞，waiter 等 cleanup
          // 真正完成才恢复。
          auto held = resources_[resource.id].lock();
          if (held) {
            resource.close_pending = true;
            defer_cleanup([held, fd] {
              const int error = close_native(fd, held->native_kind);
              {
                std::lock_guard lock(held->owner->mutex_);
                held->close_pending = false;
                held->owner->finish_close_waiters(*held, error);
              }
            });
            return;
          }
        }
        (void)close_native(fd, resource.native_kind);
      }
    }
    finish_close_waiters(resource, 0);
  }

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

  /** @brief 锁内释放结果和资源租约，然后才开放下一代 slot。 */
  void recycle_completed(operation_state& op) noexcept {
    op.allocated = false;
    op.accepted = false;
    op.target = {};
    if (op.slot == UINT32_MAX) {
      // 独立控制槽保持原有完整清理顺序，再销毁节点；本优化只针对可复用普通槽。
      op.request = io_request{};
      native_controls_.erase(op.token.value);
      return;
    }
    // 最终 native release/文件 job 已结束，且 callback
    // 已返回，才释放本代拥有参数。 空容器 swap 实际释放容量，不能 clear
    // 后长期保留很大的路径或 iovec 存储。
    op.request.resource.reset();                    // 与原 request={} 相同，此时解除稳定资源租约。
    std::vector<iovec>{}.swap(op.request.vectors);  // 描述数组拥有结束，payload 从来只是借用。
    std::string{}.swap(op.request.path);            // 空字符串接管旧动态块，在本语句末释放。
    std::string{}.swap(op.request.path2);           // 第二路径同样不保留上一代容量。
    // 其余字段仍保持合法已初始化表示，但 !allocated 的槽不允许读取它们发起 IO。
    // 下一次 prepare_locked 先完整移动赋值 request，再发布 allocated=true；
    // lookup/取消/关闭/deadline 扫描也都先校验
    // allocated，因此不继承旧控制状态。
    free_.push_back(op.slot);  // 原请求已解除全部拥有责任，现在才允许下一代领取。
  }

  /** @brief 域锁内独占领取本 token 完成；不回调、不回收、不移动稳定请求。
   * @return 空快照表示未完成或已经由其他 publisher 领取，不能重复交付。
   */
  claimed_completion claim_completed_locked(operation_token token) noexcept {
    auto* operation = lookup(token);  // 校验 allocated、slot 与完整 generation。
    if (!operation || !operation->queued_completion)
      return {};                   // driver 已领取时由它负责发布，不能重复交付。
    remove_completed(*operation);  // 从完成链独占摘除，仍 allocated 到 callback 返回。
    // callback 以前复制原目标/结果；未知消费者可在 callback 中销毁自己的
    // awaiter。
    return {operation, operation->target, operation->result, operation->transferred};
  }

  /** @brief 锁外交付已领取快照，callback 返回以后沿原域锁回收稳定槽。
   * @details 本地成功机会退出其 driver/session/domain
   * 后才调用；其他入口保持原作用域合同。
   */
  void publish_claimed(claimed_completion delivery) noexcept {
    if (delivery.target.publish)
      delivery.target.publish(delivery.target.consumer,
                              delivery.result,
                              delivery.transferred);  // 原 arming gate 同步观察 completed。
    {
      std::lock_guard lock(mutex_);
      // callback 返回后才回收稳定请求，下一代槽不能在发布者仍读取参数时复用。
      recycle_completed(*delivery.operation);
      if (free_.size() + retired_slots_ == operations_.size())
        quiescent_cv_.notify_all();  // 完全保留原回收后的控制面排空通知。
    }
  }

  /** @brief 只交付 submit 当前 token 的即时完成，其他完成留给其 owner driver。
   */
  void publish_one(operation_token token) noexcept {
    claimed_completion delivery;
    {
      std::lock_guard lock(mutex_);
      delivery = claim_completed_locked(token);  // 原正常入口复用完全相同的唯一领取协议。
    }
    if (delivery.operation)
      publish_claimed(delivery);  // 原锁外 callback 与回锁 recycle 顺序不变。
  }

  /** @brief driver 每次领取最多 64 个责任、锁外发布，再一次批量回收。
   * @details 消费者可以重入或恢复后销毁
   * awaiter；全部输出先复制，槽只在回调返回后复用。 callbacks
   * 新增的完成仍挂在域链表，后续批次继续处理，预算限制保持不变。
   */
  std::size_t publish_completed(std::size_t budget) noexcept {
    struct delivery {
      operation_state* operation;
      void* consumer;

      void (*publish)(void*, std::int64_t, std::uint64_t) noexcept;

      std::int64_t result;
      std::uint64_t transferred;
    };

    // 每个领取槽都完整写出全部字段；只访问 count 内记录，不清零空批次。
    // 将输出字段独立保存，避免 completion_target 的默认成员初始化清零未使用槽。
    std::array<delivery, 64> batch;  // 固定栈批次，不分配任何容器或修改公开 target 默认值。
    std::size_t published = 0;
    while (published < budget) {
      std::size_t count = 0;
      {
        std::lock_guard lock(mutex_);
        const auto limit = std::min(batch.size(), budget - published);
        while (count < limit && completed_head_) {
          auto* operation = completed_head_;
          remove_completed(*operation);  // 锁内取得独占发布责任，其他 publisher
          // 不能重复领取。
          batch[count++] = {operation,
                            operation->target.consumer,
                            operation->target.publish,
                            operation->result,
                            operation->transferred};
        }
        if (!count) {
          flush_deferred_cleanup();  // 仅清理 lane
          // 饱和时有工作，不新增普通空批次锁。
          break;
        }
      }
      for (std::size_t i = 0; i < count; ++i) {
        const auto entry = batch[i];  // 之后 callback 可销毁 consumer，不能再访问其 awaiter。
        if (entry.publish)
          entry.publish(entry.consumer, entry.result, entry.transferred);
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

  /** @brief 域锁内把持久清理队列转交专属 lane，永不在 IO worker 执行 close。 */
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

  void update_deadline(std::chrono::steady_clock::time_point time) noexcept {
    if (!next_deadline_ || time < *next_deadline_)
      next_deadline_ = time;
  }

  void expire_deadlines() noexcept {
    if (!next_deadline_)
      return;  // 没有任何登记的 IO deadline
    // 时不读取时钟；域锁保持判断与登记互斥。
    const auto now = std::chrono::steady_clock::now();  // 只有真实 deadline 才检查到期时间。
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
          op.cancellation = ETIMEDOUT;  // 已接受的 user/close/shutdown 取消原因优先保留。
        cancel_operation(op);
      } else
        update_deadline(*op.request.deadline);
    }
  }

  backend_box backend_;
  std::deque<operation_token> native_pending_;
  std::unordered_map<std::uint64_t, std::unique_ptr<operation_state>> native_controls_;
  std::uint32_t next_control_{};  ///< 独立控制代际不复用；不侵占业务token高位。
  const std::size_t max_resources_;
  // 资源租约释放可能在当前状态转换里重入注销，因此使用可重入控制面锁。
  mutable std::recursive_mutex mutex_;
  std::mutex driver_mutex_;
  std::condition_variable_any quiescent_cv_;
  std::vector<std::unique_ptr<operation_state>> operations_;
  std::vector<std::uint32_t> free_;
  std::size_t retired_slots_{};  ///< generation 用尽的槽，既非可用容量也非在途操作。
  std::unordered_map<std::uint64_t, std::weak_ptr<resource_state>> resources_;
  std::unordered_map<native_descriptor, std::weak_ptr<resource_state>> descriptors_;
  std::uint64_t next_resource_{};
  operation_state* completed_head_{};
  operation_state* completed_tail_{};
  std::optional<std::chrono::steady_clock::time_point> next_deadline_;
  std::atomic<bool> stopped_{};
  int fatal_error_{};  ///< 首次永久驱动故障；在内核引用排空前禁止结果回收。
  bool draining_{};
  std::stop_source stop_;
  std::shared_ptr<execution::blocking_executor> fs_, dns_, cleanup_;
  bool own_fs_{}, own_dns_{}, own_cleanup_{};
  std::shared_ptr<io_placement_group> placement_;
  std::atomic<std::size_t> publishers_{};
  std::unordered_map<std::uint64_t, ::faio::move_only_function<void()>> shutdown_cleanups_;
  std::uint64_t next_cleanup_{};
  std::deque<std::shared_ptr<::faio::move_only_function<void()>>> deferred_cleanup_;
};

inline std::shared_ptr<io_domain> io_placement_group::select() noexcept {
  std::lock_guard lock(mutex_);
  for (std::size_t inspected = 0; inspected < domains_.size(); ++inspected) {
    const auto index = next_++ % domains_.size();
    if (auto domain = domains_[index].lock(); domain && !domain->stopped())
      return domain;  // 返回拥有型租约，adopt 完成前目标 domain 不会析构。
  }
  return {};  // 全组已停止/过期，调用方的原生 guard 保留失败关闭责任。
}

inline resource_ptr io_placement_group::find_resource(native_descriptor fd,
                                                      const io_domain* skipped) noexcept {
  // 域数量在构造后固定；每个 weak_ptr 的读取仍必须在组锁内串行。
  for (std::size_t index = 0; index < domains_.size(); ++index) {
    std::shared_ptr<io_domain> domain;  // 临时拥有域，离开组锁后仍可安全读取其资源表。
    {
      std::lock_guard lock(mutex_);  // 组锁只负责提升弱租约，不进入任何域方法。
      domain = domains_[index].lock();
    }
    if (!domain || domain.get() == skipped)
      continue;  // 过期域与已经查询过的本域没有额外锁或注册副作用。
    if (auto resource = domain->find_registered_resource(fd))
      return resource;  // 此时只持有资源租约，不保留组锁或域锁。
  }
  return {};  // 完全未知的 raw fd 继续沿调用方域的现有借用注册路径处理。
}

inline resource_state::~resource_state() {
  if (owner)
    owner->release_resource(*this);
  else if (owns_handle) {
    const native_descriptor fd = handle.exchange(-1);
    if (fd >= 0) {
      if (native_kind == native_handle_kind::windows_handle)
        (void)::CloseHandle(reinterpret_cast<HANDLE>(fd));
      else
        (void)::closesocket(static_cast<SOCKET>(fd));
    }
  }
}

inline void resource_state::request_shutdown_write() noexcept {
  if (owner)
    owner->request_shutdown_write(*this);
}

inline resource_ptr adopt_resource(const io_context& context,
                                   native_descriptor fd,
                                   bool owns = true,
                                   bool regular = false) {
  if (context)
    return context.domain()->adopt(fd, owns, regular);
  auto resource = std::make_shared<resource_state>();
  resource->handle = fd;
  resource->owns_handle = owns;
  resource->regular_file = regular;
  resource->native_kind =
      regular ? native_handle_kind::windows_handle : native_handle_kind::windows_socket;
  return resource;
}

template <class F>
auto with_resource(const resource_ptr& resource,
                   Interest interest,
                   F&& function,
                   void* reservation = nullptr) -> std::invoke_result_t<F> {
  using result_type = std::invoke_result_t<F>;
  if (!resource)
    return result_type{std::unexpected{make_error(EBADF)}};
  if (resource->owner)
    return resource->owner->with_resource(
        resource, interest, std::forward<F>(function), reservation);
  if (resource->fd() < 0)
    return result_type{std::unexpected{make_error(EBADF)}};
  return std::invoke(std::forward<F>(function));
}
}  // namespace detail

inline io_context io_context::current() {
  return detail::current_domain ? io_context{detail::current_domain->shared_from_this()}
                                : io_context{};
}

inline bool io_context::stopped() const noexcept {
  return !domain_ || domain_->stopped();
}

inline std::stop_token io_context::stop_token() const noexcept {
  return domain_ ? domain_->stop_token() : std::stop_token{};
}

inline detail::backend_statistics io_context::statistics() const noexcept {
  return domain_ ? domain_->statistics() : detail::backend_statistics{};
}

inline execution::blocking_executor_ref io_context::blocking() const noexcept {
  return domain_ ? domain_->blocking() : execution::blocking_executor_ref{};
}

inline execution::blocking_executor_ref io_context::cleanup() const noexcept {
  return domain_ ? domain_->cleanup() : execution::blocking_executor_ref{};
}

inline execution::blocking_executor_ref io_context::resolver() const noexcept {
  return domain_ ? domain_->resolver() : execution::blocking_executor_ref{};
}

inline io_context io_context::balanced_context() const noexcept {
  return domain_ ? domain_->balanced_context() : io_context{};
}

inline void io_context::defer_cleanup(::faio::move_only_function<void()> cleanup) const noexcept {
  if (domain_)
    domain_->defer_cleanup(std::move(cleanup));
  else
    cleanup();
}

inline std::uint64_t io_context::register_shutdown_cleanup(
    ::faio::move_only_function<void()> cleanup) const {
  return domain_ ? domain_->register_shutdown_cleanup(std::move(cleanup)) : 0;
}

inline void io_context::unregister_shutdown_cleanup(std::uint64_t token) const noexcept {
  if (domain_)
    domain_->unregister_shutdown_cleanup(token);
}
}  // namespace faio::io

namespace faio::io::detail {
/** @brief 组合 read_exact/write_all 的方向租约；持有资源，销毁自动释放 gate。
 */
class direction_lease {
 public:
  direction_lease(resource_ptr resource, Interest interest, void* owner) noexcept
      : resource_(std::move(resource)), interest_(interest), owner_(owner) {}

  direction_lease(const direction_lease&) = delete;

  direction_lease& operator=(const direction_lease&) = delete;

  direction_lease(direction_lease&& other) noexcept
      : resource_(std::move(other.resource_)), interest_(other.interest_), owner_(other.owner_) {}

  ~direction_lease() {
    if (resource_ && resource_->owner)
      resource_->owner->unreserve(*resource_, interest_, owner_);
  }

 private:
  resource_ptr resource_;
  Interest interest_;
  void* owner_;
};

/**
 * @brief 组合协程内部的借用方向释放守卫，不重复持有资源引用计数。
 * @param resource 由同一协程帧的按值 resource_ptr 参数拥有的稳定控制块。
 * @param interest 本组合操作持有的方向；析构仍进入原 domain 的线性化锁。
 * @param owner 位于同一组合帧中的稳定方向身份。
 * @details 仅适用于资源参数在整个守卫生命周期内不被移动/清空的内部组合实现。
 *          局部守卫在退出函数体时先销毁，协程参数在 promise 销毁后才销毁；
 *          所以取消、异常和正常返回都先释放方向，再归还最后资源租约。
 *          对外取得方向租约仍返回拥有型 direction_lease，保持原所有权语义。
 */
class borrowed_direction_lease {
 public:
  borrowed_direction_lease(resource_state& resource, Interest interest, void* owner) noexcept
      : resource_(resource), interest_(interest), owner_(owner) {}

  borrowed_direction_lease(const borrowed_direction_lease&) = delete;

  borrowed_direction_lease& operator=(const borrowed_direction_lease&) = delete;

  ~borrowed_direction_lease() {
    // owner 在首次 bind 发布后不再变化；只取得一次稳定的 domain 指针。
    if (auto* domain = resource_.owner.get())
      domain->unreserve(resource_, interest_,
                        owner_);  // 复用半关闭和身份匹配协议。
  }

 private:
  resource_state& resource_;  ///< 只借用地址；创建处的协程参数保持强拥有租约。
  Interest interest_;         ///< 不更改方向排他范围和释放的线性化点。
  void* owner_;               ///< 无论首操作是否取得方向，都只释放匹配的身份。
};

inline expected<direction_lease> reserve_direction(resource_ptr resource,
                                                   Interest interest,
                                                   void* owner) noexcept {
  if (!resource)
    return std::unexpected{make_error(EBADF)};
  if (!resource->owner) {
    // 与普通 await 一致，runtime 外导入的资源由首次执行时的 context 固定归属。
    auto context = io_context::current();
    if (!context)
      return std::unexpected{make_error(ECANCELED)};
    try {
      context.domain()->bind(resource);
    } catch (const std::system_error& error) {
      if (error.code().value() != EXDEV || !resource->owner)
        return std::unexpected{make_error(error.code().value())};
      // 并发首次绑定已由其他 worker 获胜，后续 reserve 使用它发布的不可变
      // owner。
    } catch (...) {
      return std::unexpected{make_error(ENOMEM)};
    }
  }
  if (auto result = resource->owner->reserve(*resource, interest, owner); !result)
    return std::unexpected{result.error()};
  return direction_lease{std::move(resource), interest, owner};
}
}  // namespace faio::io::detail
