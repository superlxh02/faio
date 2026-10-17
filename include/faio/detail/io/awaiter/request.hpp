/**
 * @file faio/detail/io/awaiter/request.hpp
 * @brief 跨平台异步 IO：统一请求与协程注册者。
 * @details 构造阶段只保存参数，真正准备与提交发生在挂起阶段。借用负载与输出存储须保持到安全完成，立即完成和跨线程完成通过统一握手避免重复恢复。
 */
#pragma once
#include "faio/detail/io/traits/request.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/task_context.hpp"
#include "faio/detail/io/engine.hpp"
#include <atomic>
#include <concepts>
#include <coroutine>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include "faio/detail/io/platform/error.hpp"

namespace faio::io::detail {
/** @brief 后端中立 IO 协程桥；构造保存参数，await_suspend 才准备/提交稳定状态。
 * @details arming/suspended/completed gate 保证立即完成及跨线程完成只恢复一次。
 */
template <class IO, class Request = io_request>
class IORegistrantAwaiter {
  /// @brief 仅允许两个完整定义的拥有策略。
  static_assert(std::same_as<Request, io_request>
                || std::same_as<Request,
                                scalar_io_request>);
 public:
  using io_registrant_base_type =
      IORegistrantAwaiter;  ///< 精确公开基类别名，供包装约束识别两策略。

  explicit IORegistrantAwaiter(Request request,
                               io_context context = {},
                               bool cancellable = true) noexcept
      : request_(std::move(request)), context_(std::move(context)), cancellable_(cancellable) {}

  /** @brief 请求工厂直接构造 awaiter 的拥有成员，免去临时请求到成员的整块移动。
   * @details 工厂只创建参数，不注册句柄或提交 IO；返回同类型 prvalue
   * 直接初始化成员。 原按值入口与所有公开请求/取消/选项接口保留，只有内部
   * scalar 构造选此入口。
   */
  template <class RequestFactory>
    requires requires(RequestFactory&& factory) {
      { std::forward<RequestFactory>(factory)() } -> std::same_as<Request>;
    }
  explicit IORegistrantAwaiter(
      RequestFactory&& factory,
      io_context context = {},
      bool cancellable = true) noexcept(noexcept(std::forward<RequestFactory>(factory)()))
      /// @brief C++17 同类型 prvalue 直接构造此成员。
      : request_(std::forward<RequestFactory>(factory)()),
        context_(std::move(context)),
        cancellable_(cancellable) {}

  IORegistrantAwaiter(const IORegistrantAwaiter&) = delete;

  IORegistrantAwaiter& operator=(const IORegistrantAwaiter&) = delete;

  IORegistrantAwaiter(IORegistrantAwaiter&& other) noexcept
      : _user_data(other._user_data),
        request_(std::move(other.request_)),
        context_(std::move(other.context_)),
        cancellable_(other.cancellable_) {}

  IORegistrantAwaiter& operator=(IORegistrantAwaiter&&) = delete;

  /// @brief 准备阶段不偷偷发起 IO。
  bool await_ready() const noexcept { return false; }

  /// @brief 检查本等待器是否已经取得非零的稳定操作令牌。
  bool was_prepared() const noexcept { return token_.value != 0; }

  /** @brief 保存恢复目标并提交 stable token，只有 gate 成功转入 suspended
   * 才真正挂起。
   * @param handle 等待本次结果的协程；其数据缓冲区必须一直存活到 await_resume。
   * @return false 表示立即完成/失败，true 表示以后通过 scheduler 恢复一次。
   * @details 发布者可在 submit 内同步完成，也可来自其他 worker 或 blocking
   * lane； gate 的 release/acquire 将结果写入与 await_resume 的读取串联起来。
   */
  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    /// @brief 优先沿已有资源读取固定 IO 域归属，避免普通读写重复取得默认上下文共享租约。
    auto* domain = request_.resource ? request_.resource->owner.get() : nullptr;
    /// @brief 只有尚未归属的资源/raw fd 才需要取得默认 context 租约。
    io_context context;
    /// @brief 尚未绑定资源时才解析显式或当前线程默认上下文，随后首次绑定固定归属。
    if (!domain) {
      /// @brief 显式指定上下文优先；没有显式上下文时使用当前运行时线程的 IO 绑定。
      context = context_ ? context_ : io_context::current();
      /// @brief 没有资源归属也没有可用上下文时，在接纳请求之前报告失败并直接继续协程。
      if (!context) {
        _user_data.result = -ECANCELED;
        return false;
      }
      domain = context.domain().get();
    }
    /// @brief raw fd 不携带 shard 身份；协程恢复到其他 worker
    /// 后必须复用原来的稳定资源。 typed IO 和 File 的 bypass
    /// 请求完全跳过此冷路径，不增加热路径控制面锁。
    if (!request_.resource && !request_.bypass_resource_registration && request_.fd >= 0
        && request_.kind != operation_kind::open && request_.kind != operation_kind::open2
        && request_.kind != operation_kind::socket) {
      /// @brief 显式 context 仍限定调用方选择的域；隐式 current 只在同一 runtime
      /// 内查找。
      request_.resource = domain->find_raw_resource(request_.fd, !context_);
      /// @brief 后续 prepare/取消都进入原 owner。
      if (request_.resource)
        domain = request_.resource->owner.get();
    }
    /// @brief 已绑定请求本身拥有 resource->owner；瞬时路径可安全借用指针，免反复
    /// shared_ptr 计数。 没有默认 domain 的原生导入资源只在第一次 await
    /// 确定归属。
    if (request_.resource && !request_.resource->owner) {
      try {
        domain->bind(request_.resource);
      } catch (const std::system_error& e) {
        if (e.code().value() != EXDEV || !request_.resource->owner) {
          _user_data.result = -e.code().value();
          return false;
        }
        /// @brief 另一个 worker
        /// 已完成首次绑定时跟随该稳定归属，不把并发首次使用当成错误。
      } catch (...) {
        _user_data.result = -ENOMEM;
        return false;
      }
    }
    /// @brief 并发首次绑定后重新读取资源的最终归属，跟随已经发布的稳定 owner。
    if (request_.resource && request_.resource->owner)
      domain = request_.resource->owner.get();
    /// @brief arming 期间借用 TLS，免即时 IO 引用计数竞争。
    const auto& stop = ::faio::detail::current_stop_token;

    if (const auto immediate =
            domain->try_immediate(request_, cancellable_ && stop.stop_requested())) {
      _user_data = {immediate->result, immediate->transferred};
      /// @brief 没有留存异步引用，当前帧直接继续执行。
      gate_.store(2,
                  std::memory_order_release);
      return false;
    }
    /// @brief 瞬时结果没有登记消费者；只有进入稳定槽前才保存真实恢复目标。
    /// 必须先初始化这两个成员，再调用可能同步发布结果的 prepare_submit。
    handle_ = handle;
    /// @brief 线程绑定仍是进入 await_suspend 时的稳定借用。
    scheduler_ = ::faio::detail::current_scheduler();

    /// @brief 真正异步路径先建立拥有型租约，再移动请求到稳定槽。
    domain_ = domain->shared_from_this();
    /// @brief 保存准备阶段错误码，准备失败表示域尚未接管异步完成责任。
    int error{};
    /// @brief 同一锁内准备/任务取消/接受，stable request/SQE/CQE 管线不变。
    const auto token = [&] {
      if constexpr (std::same_as<Request, io_request>) {
        /// @brief 原通用桥继续直接移动完整拥有请求；所有冷 opcode/SendZC 保持原协议。
        return domain->prepare_submit(
            std::move(request_), {this, &publish}, error, stop, cancellable_);
      } else {
        /// @brief 只有真实 native 或 readiness 等待才初始化冷字段；不复制资源租约。
        auto full = std::move(request_).into_request();
        const auto prepared =
            domain->prepare_submit(std::move(full), {this, &publish}, error, stop, cancellable_);
        /// @brief 未接受时还原原 awaiter 的拥有租约。
        if (!prepared.value)
          request_.resource = std::move(full.resource);
        /// @brief prepare 失败以前不移动输入；与原完整桥相同，命名 awaiter
        /// 仍可保有参数。
        return prepared;
      }
    }();
    /// @brief 零令牌表示未取得稳定槽，等待器保留失败结果并让当前协程直接继续。
    if (!token.value) {
      /// @brief 把准备失败转换为统一的负错误码，未进入异步路径时不登记恢复责任。
      _user_data.result = -error;
      return false;
    }
    /// @brief token 含 slot 与 generation，取消回调不会借用 operation 地址。
    token_ = token;
    /// @brief prepare_submit 已在 accepted 前观察任务停止并保存 sticky cancel。
    /// 锁外即时发布可能已将 gate 置 completed；arming
    /// 期间绝不提前恢复/销毁当前帧。 即时成功免去共享根 stop_state
    /// 的注册/析构锁；pending 仍严格订阅停止。 gate 尚为
    /// arming，构造回调期间发生完成也不能提前恢复/销毁当前帧。
    if (gate_.load(std::memory_order_acquire) != 2 && cancellable_ && stop.stop_possible())
      stop_callback_.emplace(stop, cancel_callback{domain_, token});
    /// @brief CAS 成功后另一线程可立即恢复/销毁帧；之后只返回局部值，不能再访问成员。
    unsigned char arming = 0;
    /// @brief 原子交接挂起资格；失败说明结果已到达，由当前执行流读取，成功后禁止继续访问等待器成员。
    return gate_.compare_exchange_strong(arming, 1, std::memory_order_acq_rel);
  }

  /// @brief 保存单调时钟绝对截止时间，真正超时仲裁由提交与驱动协议完成。
  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) && noexcept -> IO {
    /// @brief 仅保存截止时间元数据，域在提交和驱动阶段统一决定超时原因。
    request_.deadline = deadline;
    return std::move(*static_cast<IO*>(this));
  }

  /// @brief 保存单调时钟绝对截止时间，真正超时仲裁由提交与驱动协议完成。
  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) & noexcept -> IO& {
    /// @brief 仅保存截止时间元数据，域在提交和驱动阶段统一决定超时原因。
    request_.deadline = deadline;
    return *static_cast<IO*>(this);
  }

  /// @brief 把相对时长转换为单调时钟截止时间并保存到本次等待。
  auto set_timeout(std::chrono::milliseconds duration) && noexcept -> IO {
    return std::move(*this).set_timeout_at(std::chrono::steady_clock::now() + duration);
  }

  /// @brief 把相对时长转换为单调时钟截止时间并保存到本次等待。
  auto set_timeout(std::chrono::milliseconds duration) & noexcept -> IO& {
    return set_timeout_at(std::chrono::steady_clock::now() + duration);
  }

  /// @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。
  auto reservation(void* owner) && noexcept -> IO {
    /// @brief 保存地址稳定的组合操作身份，后续方向 gate 验证必须与该身份一致。
    request_.reservation = owner;
    return std::move(*static_cast<IO*>(this));
  }

  /// @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。
  auto reservation(void* owner) & noexcept -> IO& {
    /// @brief 保存地址稳定的组合操作身份，后续方向 gate 验证必须与该身份一致。
    request_.reservation = owner;
    return *static_cast<IO*>(this);
  }

  /** @brief 组合操作将方向租约取得与首个真实 IO 合并；不增加协程或后台线程。
   * @param owner 地址稳定的组合帧身份，必须由 direction_lease
   * 在所有退出路径释放。
   * @details 原生后端在 submit 的同一锁内取得租约；readiness 后端在瞬时 attempt
   *          前取得租约，EAGAIN/短写以后仍保持排他。此方法本身不取得执行权。
   */
  auto establish_reservation(void* owner) && noexcept -> IO {
    /// @brief 只保存身份，await_suspend 才建立方向租约。
    request_.reservation = owner;
    /// @brief 标记首个真实请求需要原子取得方向预留，本方法自身不执行 IO。
    request_.establish_reservation = true;
    return std::move(*static_cast<IO*>(this));
  }

  /// @brief 声明在首个真实 IO 的仲裁点取得组合操作方向预留。
  auto establish_reservation(void* owner) & noexcept -> IO& {
    /// @brief 保存地址稳定的组合操作身份，后续方向 gate 验证必须与该身份一致。
    request_.reservation = owner;
    /// @brief 标记首个真实请求需要原子取得方向预留，本方法自身不执行 IO。
    request_.establish_reservation = true;
    return *static_cast<IO*>(this);
  }

  /** @brief 为字节流声明空缓冲区是立即成功的无操作；datagram
   * 接口必须保持默认关闭。
   * @param enabled 是否应用流式空读写规则；真实非空请求始终执行原系统调用。
   */
  auto empty_success(bool enabled = true) && noexcept -> IO {
    /// @brief 显式保存流式空请求规则，避免把合法零长度报文误当无操作。
    request_.empty_success = enabled;
    return std::move(*static_cast<IO*>(this));
  }

  /// @brief 声明字节流空负载可立即成功；数据报不能隐式套用此规则。
  auto empty_success(bool enabled = true) & noexcept -> IO& {
    /// @brief 显式保存流式空请求规则，避免把合法零长度报文误当无操作。
    request_.empty_success = enabled;
    return *static_cast<IO*>(this);
  }

  /// @brief 为本次请求保存共享资源租约，固定句柄身份及所属 IO 域。
  auto with_resource(resource_ptr resource) && noexcept -> IO {
    /// @brief 转入共享资源租约，异步完成以前固定原生句柄身份及其归属。
    request_.resource = std::move(resource);
    return std::move(*static_cast<IO*>(this));
  }

  /// @brief 为本次请求保存共享资源租约，固定句柄身份及所属 IO 域。
  auto with_resource(resource_ptr resource) & noexcept -> IO& {
    /// @brief 转入共享资源租约，异步完成以前固定原生句柄身份及其归属。
    request_.resource = std::move(resource);
    return *static_cast<IO*>(this);
  }

  /// @brief 显式指定请求上下文；已经绑定资源的归属仍按资源协议校验。
  auto with_context(io_context context) && noexcept -> IO {
    /// @brief 保存显式上下文租约，真正提交时再与既有资源归属核对。
    context_ = std::move(context);
    return std::move(*static_cast<IO*>(this));
  }

  /// @brief 显式指定请求上下文；已经绑定资源的归属仍按资源协议校验。
  auto with_context(io_context context) & noexcept -> IO& {
    /// @brief 保存显式上下文租约，真正提交时再与既有资源归属核对。
    context_ = std::move(context);
    return *static_cast<IO*>(this);
  }

 protected:
  struct user_data {
    std::int64_t result{};  ///< 已完成操作的返回值；具体错误解释遵循所属接口约定。
    std::uint64_t transferred{};  ///< 操作已完成的字节数，失败或取消时仍保留有效进度。
  } _user_data{};  ///< 统一完成结果槽，发布握手前写入结果及字节进度。

  Request request_;  ///< 默认策略保留完整请求；Recv/Send 采用紧凑拥有描述。

 private:
  struct cancel_callback {
    std::shared_ptr<io_domain> domain;  ///< 共享持有所属 IO 域，覆盖取消回调与完成发布。
    operation_token token;  ///< 包含槽位与代际的操作身份，避免取消或完成命中过期操作。

    void operator()() const noexcept { domain->request_cancel(token); }
  };

  /// @brief 在结果写入后发布完成握手，只有已经挂起的等待者才重新入队。
  static void publish(void* consumer, std::int64_t result, std::uint64_t transferred) noexcept {
    /// @brief 还原用户态消费者，内核事件本身始终只携带资源键或操作代际。
    auto& awaiter = *static_cast<IORegistrantAwaiter*>(consumer);
    /// @brief release gate 之前完整写入结果。
    awaiter._user_data = {result, transferred};
    /// @brief 唯一完成边界。
    const auto previous = awaiter.gate_.exchange(2, std::memory_order_acq_rel);
    /// @brief 只有完成前已经取得挂起资格的协程才需要调度；登记期完成由 await_suspend 自行继续。
    if (previous == 1) {
      /// @brief 已挂起才调度；arming 状态由 await_suspend 自身继续执行。
      /// @brief schedule 后 awaiter 可能立刻销毁，先复制引用。
      auto scheduler = awaiter.scheduler_;
      /// @brief 之后恢复线程可以独占协程帧，发布者不再访问帧。
      const auto handle = awaiter.handle_;
      try {
        /// @brief runtime支持时进入本地快速槽/批次FIFO，普通调度器保持原入口。
        scheduler.schedule_io(
            handle);
      } catch (...) {
        std::terminate();
      }
      /// @brief schedule 接管后不再访问 awaiter，恢复线程可能已销毁它。
    }
  }

  io_context context_;  ///< 持有所属 IO 上下文租约，用于资源归属、服务选择与安全排空。
  bool cancellable_{true};  ///< 是否订阅任务取消；必须排空的清理请求可显式关闭取消。
  std::shared_ptr<io_domain> domain_;  ///< cancel callback 不借用 runtime worker 地址。
  operation_token token_{};  ///< 包含槽位与代际的操作身份，避免取消或完成命中过期操作。
  scheduler_ref scheduler_;  ///< 捕获的目标调度器，跨线程完成时将续体投递回所属运行时。
  std::coroutine_handle<> handle_{};  ///< 当前持有或等待恢复的协程帧句柄，所有权遵循所属等待协议。
  std::atomic<unsigned char> gate_{0};  ///< 0 表示登记中，1 表示已挂起，2 表示已完成。
  std::optional<std::stop_callback<cancel_callback>> stop_callback_;  ///< 按需就地保存的停止回调，析构时注销并等待在途回调结束。
};

}
