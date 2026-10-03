#pragma once
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
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_error.hpp"
#endif
namespace faio::io::detail {
/** @brief 原生错误在 IO 公共边界解码，保留来源与实际传输进度。 */
inline Error decode_io_error(int value, std::uint64_t progress = 0) noexcept {
#if defined(_WIN32)
  return windows::make_io_error(value, progress);
#else
  return Error{value, progress};
#endif
}

/** @brief 后端中立 IO 协程桥；构造保存参数，await_suspend 才准备/提交稳定状态。
 * @details arming/suspended/completed gate 保证立即完成及跨线程完成只恢复一次。
 */
template <class IO, class Request = io_request>
class IORegistrantAwaiter {
  static_assert(std::same_as<Request, io_request>
                || std::same_as<Request,
                                scalar_io_request>);  // 仅允许两个完整定义的拥有策略。
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
      : request_(std::forward<RequestFactory>(factory)()),  // C++17 同类型 prvalue 直接构造此成员。
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

  bool await_ready() const noexcept { return false; }  // 准备阶段不偷偷发起 IO。

  bool was_prepared() const noexcept { return token_.value != 0; }

  /** @brief 保存恢复目标并提交 stable token，只有 gate 成功转入 suspended
   * 才真正挂起。
   * @param handle 等待本次结果的协程；其数据缓冲区必须一直存活到 await_resume。
   * @return false 表示立即完成/失败，true 表示以后通过 scheduler 恢复一次。
   * @details 发布者可在 submit 内同步完成，也可来自其他 worker 或 blocking
   * lane； gate 的 release/acquire 将结果写入与 await_resume 的读取串联起来。
   */
  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    auto* domain = request_.resource ? request_.resource->owner.get() : nullptr;
    io_context context;  // 只有尚未归属的资源/raw fd 才需要取得默认 context 租约。
    if (!domain) {
      context = context_ ? context_ : io_context::current();
      if (!context) {
        _user_data.result = -ECANCELED;
        return false;
      }
      domain = context.domain().get();
    }
    // raw fd 不携带 shard 身份；协程恢复到其他 worker
    // 后必须复用原来的稳定资源。 typed IO 和 File 的 bypass
    // 请求完全跳过此冷路径，不增加热路径控制面锁。
    if (!request_.resource && !request_.bypass_resource_registration && request_.fd >= 0
        && request_.kind != operation_kind::open && request_.kind != operation_kind::open2
        && request_.kind != operation_kind::socket) {
      // 显式 context 仍限定调用方选择的域；隐式 current 只在同一 runtime
      // 内查找。
      request_.resource = domain->find_raw_resource(request_.fd, !context_);
      if (request_.resource)
        domain = request_.resource->owner.get();  // 后续 prepare/取消都进入原 owner。
    }
    // 已绑定请求本身拥有 resource->owner；瞬时路径可安全借用指针，免反复
    // shared_ptr 计数。 没有默认 domain 的原生导入资源只在第一次 await
    // 确定归属。
    if (request_.resource && !request_.resource->owner) {
      try {
        domain->bind(request_.resource);
      } catch (const std::system_error& e) {
        if (e.code().value() != EXDEV || !request_.resource->owner) {
          _user_data.result = -e.code().value();
          return false;
        }
        // 另一个 worker
        // 已完成首次绑定时跟随该稳定归属，不把并发首次使用当成错误。
      } catch (...) {
        _user_data.result = -ENOMEM;
        return false;
      }
    }
    if (request_.resource && request_.resource->owner)
      domain = request_.resource->owner.get();
    const auto& stop = ::faio::detail::current_stop_token;  // arming 期间借用 TLS，免即时 IO
    // 引用计数竞争。
    if (const auto immediate =
            domain->try_immediate(request_, cancellable_ && stop.stop_requested())) {
      _user_data = {immediate->result, immediate->transferred};
      gate_.store(2,
                  std::memory_order_release);  // 没有留存异步引用，当前帧直接继续执行。
      return false;
    }
    // 瞬时结果没有登记消费者；只有进入稳定槽前才保存真实恢复目标。
    // 必须先初始化这两个成员，再调用可能同步发布结果的 prepare_submit。
    handle_ = handle;
    scheduler_ = ::faio::detail::current_scheduler();  // 线程绑定仍是进入 await_suspend
    // 时的稳定借用。
    domain_ = domain->shared_from_this();  // 真正异步路径先建立拥有型租约，再移动请求到稳定槽。
    int error{};
    const auto token = [&] {
      if constexpr (std::same_as<Request, io_request>) {
        // 原通用桥继续直接移动完整拥有请求；所有冷 opcode/SendZC 保持原协议。
        return domain->prepare_submit(
            std::move(request_), {this, &publish}, error, stop, cancellable_);
      } else {
        // 只有真实 native 或 readiness 等待才初始化冷字段；不复制资源租约。
        auto full = std::move(request_).into_request();
        const auto prepared =
            domain->prepare_submit(std::move(full), {this, &publish}, error, stop, cancellable_);
        if (!prepared.value)
          request_.resource = std::move(full.resource);  // 未接受时还原原 awaiter 的拥有租约。
        // prepare 失败以前不移动输入；与原完整桥相同，命名 awaiter
        // 仍可保有参数。
        return prepared;
      }
    }();  // 同一锁内准备/任务取消/接受，stable request/SQE/CQE 管线不变。
    if (!token.value) {
      _user_data.result = -error;
      return false;
    }
    token_ = token;  // token 含 slot 与 generation，取消回调不会借用 operation 地址。
    // prepare_submit 已在 accepted 前观察任务停止并保存 sticky cancel。
    // 锁外即时发布可能已将 gate 置 completed；arming
    // 期间绝不提前恢复/销毁当前帧。 即时成功免去共享根 stop_state
    // 的注册/析构锁；pending 仍严格订阅停止。 gate 尚为
    // arming，构造回调期间发生完成也不能提前恢复/销毁当前帧。
    if (gate_.load(std::memory_order_acquire) != 2 && cancellable_ && stop.stop_possible())
      stop_callback_.emplace(stop, cancel_callback{domain_, token});
    // CAS 成功后另一线程可立即恢复/销毁帧；之后只返回局部值，不能再访问成员。
    unsigned char arming = 0;
    return gate_.compare_exchange_strong(arming, 1, std::memory_order_acq_rel);
  }

  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) && noexcept -> IO {
    request_.deadline = deadline;
    return std::move(*static_cast<IO*>(this));
  }

  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) & noexcept -> IO& {
    request_.deadline = deadline;
    return *static_cast<IO*>(this);
  }

  auto set_timeout(std::chrono::milliseconds duration) && noexcept -> IO {
    return std::move(*this).set_timeout_at(std::chrono::steady_clock::now() + duration);
  }

  auto set_timeout(std::chrono::milliseconds duration) & noexcept -> IO& {
    return set_timeout_at(std::chrono::steady_clock::now() + duration);
  }

  auto reservation(void* owner) && noexcept -> IO {
    request_.reservation = owner;
    return std::move(*static_cast<IO*>(this));
  }

  auto reservation(void* owner) & noexcept -> IO& {
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
    request_.reservation = owner;  // 只保存身份，await_suspend 才建立方向租约。
    request_.establish_reservation = true;
    return std::move(*static_cast<IO*>(this));
  }

  auto establish_reservation(void* owner) & noexcept -> IO& {
    request_.reservation = owner;
    request_.establish_reservation = true;
    return *static_cast<IO*>(this);
  }

  /** @brief 为字节流声明空缓冲区是立即成功的无操作；datagram
   * 接口必须保持默认关闭。
   * @param enabled 是否应用流式空读写规则；真实非空请求始终执行原系统调用。
   */
  auto empty_success(bool enabled = true) && noexcept -> IO {
    request_.empty_success = enabled;
    return std::move(*static_cast<IO*>(this));
  }

  auto empty_success(bool enabled = true) & noexcept -> IO& {
    request_.empty_success = enabled;
    return *static_cast<IO*>(this);
  }

  auto with_resource(resource_ptr resource) && noexcept -> IO {
    request_.resource = std::move(resource);
    return std::move(*static_cast<IO*>(this));
  }

  auto with_resource(resource_ptr resource) & noexcept -> IO& {
    request_.resource = std::move(resource);
    return *static_cast<IO*>(this);
  }

  auto with_context(io_context context) && noexcept -> IO {
    context_ = std::move(context);
    return std::move(*static_cast<IO*>(this));
  }

  auto with_context(io_context context) & noexcept -> IO& {
    context_ = std::move(context);
    return *static_cast<IO*>(this);
  }

 protected:
  struct user_data {
    std::int64_t result{};
    std::uint64_t transferred{};
  } _user_data{};

  Request request_;  ///< 默认策略保留完整请求；Recv/Send 采用紧凑拥有描述。

 private:
  struct cancel_callback {
    std::shared_ptr<io_domain> domain;
    operation_token token;

    void operator()() const noexcept { domain->request_cancel(token); }
  };

  static void publish(void* consumer, std::int64_t result, std::uint64_t transferred) noexcept {
    auto& awaiter = *static_cast<IORegistrantAwaiter*>(consumer);
    awaiter._user_data = {result, transferred};  // release gate 之前完整写入结果。
    const auto previous = awaiter.gate_.exchange(2, std::memory_order_acq_rel);  // 唯一完成边界。
    if (previous == 1) {
      // 已挂起才调度；arming 状态由 await_suspend 自身继续执行。
      auto scheduler = awaiter.scheduler_;  // schedule 后 awaiter 可能立刻销毁，先复制引用。
      const auto handle = awaiter.handle_;  // 之后恢复线程可以独占协程帧，发布者不再访问帧。
      try {
        scheduler.schedule_io(
            handle);  // runtime支持时进入本地快速槽/批次FIFO，普通调度器保持原入口。
      } catch (...) {
        std::terminate();
      }
      // schedule 接管后不再访问 awaiter，恢复线程可能已销毁它。
    }
  }

  io_context context_;
  bool cancellable_{true};
  std::shared_ptr<io_domain> domain_;  ///< cancel callback 不借用 runtime worker 地址。
  operation_token token_{};
  scheduler_ref scheduler_;
  std::coroutine_handle<> handle_{};
  std::atomic<unsigned char> gate_{0};  ///< 0=arming，1=suspended，2=completed。
  std::optional<std::stop_callback<cancel_callback>> stop_callback_;
};

/** @brief 只识别真正的 IORegistrantAwaiter 实例，防止自指别名误满足约束。 */
template <class T>
struct is_io_registrant_base : std::false_type {};

template <class IO, class Request>
struct is_io_registrant_base<IORegistrantAwaiter<IO, Request>> : std::true_type {};

/** @brief 完整/紧凑及兼容包装共享的精确 CRTP 操作约束，无运行时成本。 */
template <class T>
concept io_registrant_operation =
    requires { typename std::remove_cvref_t<T>::io_registrant_base_type; }
    && is_io_registrant_base<typename std::remove_cvref_t<T>::io_registrant_base_type>::value
    && std::derived_from<std::remove_cvref_t<T>,
                         typename std::remove_cvref_t<T>::io_registrant_base_type>;
}  // namespace faio::io::detail
