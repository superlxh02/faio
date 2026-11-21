/**
 * @file faio/detail/io/awaiter/request.hpp
 * @brief 统一请求与协程注册者。
 */
#pragma once
#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/io/traits/request.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/context/task_context.hpp"
#include "faio/detail/io/core/domain.hpp"
#include <atomic>
#include <concepts>
#include <coroutine>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include "faio/detail/io/platform/error.hpp"

namespace faio::io::detail {
/**
 * @brief 与后端无关的 I/O 协程桥接；构造时保存参数，在 await_suspend 中准备并提交稳定状态。
 * @details arming/suspended/completed 门闩保证立即完成和跨线程完成均只恢复一次协程。
 */
template <class IO, class Request = io_request>
class IORegistrantAwaiter : protected completion_state {
  static_assert(std::same_as<Request, io_request> || std::same_as<Request, scalar_io_request>);

 public:
  using io_registrant_base_type =
      IORegistrantAwaiter; /**< 公开所用的基类别名，供包装约束识别两种策略。 */

  /** @brief 构造注册 I/O 请求的协程等待器。 */
  explicit IORegistrantAwaiter(Request request,
                               io_context context = {},
                               bool cancellable = true) noexcept
      : request_(std::move(request)), context_(std::move(context)), cancellable_(cancellable) {}

  /**
   * @brief 由请求工厂直接构造 awaiter 持有的成员，避免将临时请求整体移动到成员中。
   * @details 工厂仅创建参数，不注册句柄或提交 I/O；返回的同类型纯右值直接初始化成员。
   * 原按值入口以及公开的请求、取消和选项接口均保留，仅内部单缓冲区构造使用此入口。
   */
  template <class RequestFactory>
    requires requires(RequestFactory&& factory) {
      { std::forward<RequestFactory>(factory)() } -> std::same_as<Request>;
    }
  explicit IORegistrantAwaiter(
      RequestFactory&& factory,
      io_context context = {},
      bool cancellable = true) noexcept(noexcept(std::forward<RequestFactory>(factory)()))
      : request_(std::forward<RequestFactory>(factory)()),
        context_(std::move(context)),
        cancellable_(cancellable) {}

  /** @brief 禁止复制构造注册 I/O 请求的协程等待器。 */
  IORegistrantAwaiter(const IORegistrantAwaiter&) = delete;

  /** @brief 禁止复制赋值。 */
  IORegistrantAwaiter& operator=(const IORegistrantAwaiter&) = delete;

  /** @brief 移动构造注册 I/O 请求的协程等待器。 */
  IORegistrantAwaiter(IORegistrantAwaiter&& other) noexcept
      : request_(std::move(other.request_)),
        context_(std::move(other.context_)),
        cancellable_(other.cancellable_) {
    _user_data = other._user_data;
  }

  /** @brief 禁止移动赋值。 */
  IORegistrantAwaiter& operator=(IORegistrantAwaiter&&) = delete;

  /** @brief 准备阶段不发起 I/O。 */
  bool await_ready() const noexcept { return false; }

  /** @brief 检查本等待器是否已经取得非零的稳定操作令牌。 */
  bool was_prepared() const noexcept { return token_.value != 0; }

  /**
   * @brief 保存恢复目标并提交稳定令牌；仅在门闩成功进入 suspended 状态时挂起。
   * @param handle 等待本次结果的协程；其数据缓冲区须始终有效，直到 await_resume。
   * @return false 表示立即完成或失败，true 表示稍后由调度器恢复一次。
   * @details 发布者可在 submit 内同步完成，也可从其他 worker 或阻塞通道完成。
   * 门闩的 release/acquire 保证 await_resume 能读取已写入的结果。
   */
  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    try {
      return await_suspend_impl(
          handle,
          ::faio::detail::capture_resume_target(::faio::detail::current_scheduler(), false));
    } catch (const std::overflow_error&) {
      _user_data.result = -EAGAIN;
      return false;
    } catch (...) {
      _user_data.result = -ECANCELED;
      return false;
    }
  }

  /** @brief 按任务的静态调度器类型从注册线程捕获恢复目标。 */
  template <class Promise>
    requires ::faio::detail::has_task_scheduler_type<Promise>
  bool await_suspend(std::coroutine_handle<Promise> handle) {
    try {
      return await_suspend_impl(
          handle,
          ::faio::detail::capture_current_resume_target<typename Promise::scheduler_type>(false));
    } catch (const std::overflow_error&) {
      _user_data.result = -EAGAIN;
      return false;
    }
  }

 private:
  /** @brief 共享原请求、结果和 gate；调用方仅选择已有的完成消费者。 */
  template <class S>
  bool await_suspend_impl(std::coroutine_handle<> handle,
                          ::faio::detail::basic_resume_target<S> scheduler) noexcept {
    auto* domain = request_.resource ? request_.resource->owner.get() : nullptr;
    io_context context;
    if (!domain) {
      context = context_ ? context_ : io_context::current();
      if (!context) {
        _user_data.result = -ECANCELED;
        return false;
      }
      domain = context.domain().get();
    }
    if (!request_.resource && !request_.bypass_resource_registration && request_.fd >= 0
        && request_.kind != operation_kind::open && request_.kind != operation_kind::open2
        && request_.kind != operation_kind::socket) {
      request_.resource = domain->find_raw_resource(request_.fd, !context_);
      if (request_.resource)
        domain = request_.resource->owner.get();
    }
    if (request_.resource && !request_.resource->owner) {
      try {
        domain->bind(request_.resource);
      } catch (const std::system_error& e) {
        if (e.code().value() != EXDEV || !request_.resource->owner) {
          _user_data.result = -e.code().value();
          return false;
        }
      } catch (...) {
        _user_data.result = -ENOMEM;
        return false;
      }
    }
    if (request_.resource && request_.resource->owner)
      domain = request_.resource->owner.get();
    const auto& stop = ::faio::detail::current_thread_context().task_ctx.stop_token;

    if (const auto immediate =
            domain->try_immediate(request_, cancellable_ && stop.stop_requested())) {
      _user_data = {immediate->result, immediate->transferred};
      gate_.store(2, std::memory_order_release);
      return false;
    }
    if (!scheduler) {
      _user_data.result = -ECANCELED;
      return false;
    }
    try {
      scheduler.reserve();
      arm(scheduler, handle);
    } catch (const std::overflow_error&) {
      _user_data.result = -EAGAIN;
      return false;
    } catch (const std::logic_error&) {
      _user_data.result = -EALREADY;
      return false;
    } catch (...) {
      _user_data.result = -ENOMEM;
      return false;
    }
    const completion_target completion{*this};

    domain_ = domain->shared_from_this();
    int error{};
    const auto token = [&] {
      if constexpr (std::same_as<Request, io_request>) {
        return domain->prepare_submit(std::move(request_), completion, error, stop, cancellable_);
      } else {
        auto full = std::move(request_).into_request();
        const auto prepared =
            domain->prepare_submit(std::move(full), completion, error, stop, cancellable_);
        if (!prepared.value)
          request_.resource = std::move(full.resource);
        return prepared;
      }
    }();
    if (!token.value) {
      discard_unaccepted();
      _user_data.result = -error;
      return false;
    }
    token_ = token;
    if (gate_.load(std::memory_order_acquire) != 2 && cancellable_ && stop.stop_possible())
      stop_callback_.emplace(stop, cancel_callback{domain_, token});
    unsigned char arming = 0;
    return gate_.compare_exchange_strong(arming, 1, std::memory_order_acq_rel);
  }

 public:
  /** @brief 保存单调时钟的绝对截止时间，由提交与驱动协议仲裁超时。 */
  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) && noexcept -> IO {
    request_.deadline = deadline;
    return std::move(*static_cast<IO*>(this));
  }

  /** @brief 保存单调时钟的绝对截止时间，由提交与驱动协议仲裁超时。 */
  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) & noexcept -> IO& {
    request_.deadline = deadline;
    return *static_cast<IO*>(this);
  }

  /** @brief 将相对时长转换为单调时钟截止时间，保存到本次等待中。 */
  auto set_timeout(std::chrono::milliseconds duration) && noexcept -> IO {
    return std::move(*this).set_timeout_at(std::chrono::steady_clock::now() + duration);
  }

  /** @brief 将相对时长转换为单调时钟截止时间，保存到本次等待中。 */
  auto set_timeout(std::chrono::milliseconds duration) & noexcept -> IO& {
    return set_timeout_at(std::chrono::steady_clock::now() + duration);
  }

  /** @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。 */
  auto reservation(void* owner) && noexcept -> IO {
    request_.reservation = owner;
    return std::move(*static_cast<IO*>(this));
  }

  /** @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。 */
  auto reservation(void* owner) & noexcept -> IO& {
    request_.reservation = owner;
    return *static_cast<IO*>(this);
  }

  /**
   * @brief 将取得组合操作方向租约与首个实际 I/O 合并，不增加协程或后台线程。
   * @param owner 地址稳定的组合帧身份，须由 direction_lease 在所有退出路径上释放。
   * @details 原生后端在 submit 的同一把锁内取得租约；就绪后端在即时尝试前取得。
   * EAGAIN 或短写后仍保持排他。本方法仅作声明，不直接取得执行权。
   */
  auto establish_reservation(void* owner) && noexcept -> IO {
    request_.reservation = owner;
    request_.establish_reservation = true;
    return std::move(*static_cast<IO*>(this));
  }

  /** @brief 声明在首个实际 I/O 的仲裁点取得组合操作的方向预留。 */
  auto establish_reservation(void* owner) & noexcept -> IO& {
    request_.reservation = owner;
    request_.establish_reservation = true;
    return *static_cast<IO*>(this);
  }

  /**
   *  @brief 为字节流声明空缓冲区是立即成功的无操作；datagram
   * 接口必须保持默认关闭。
   * @param enabled 是否应用流式空读写规则；真实非空请求始终执行原系统调用。
   */
  auto empty_success(bool enabled = true) && noexcept -> IO {
    request_.empty_success = enabled;
    return std::move(*static_cast<IO*>(this));
  }

  /** @brief 声明字节流空负载可立即成功；数据报不能隐式套用此规则。 */
  auto empty_success(bool enabled = true) & noexcept -> IO& {
    request_.empty_success = enabled;
    return *static_cast<IO*>(this);
  }

  /** @brief 为本次请求保存共享资源租约，保持句柄身份及所属 I/O 域不变。 */
  auto with_resource(resource_ptr resource) && noexcept -> IO {
    request_.resource = std::move(resource);
    return std::move(*static_cast<IO*>(this));
  }

  /** @brief 为本次请求保存共享资源租约，保持句柄身份及所属 I/O 域不变。 */
  auto with_resource(resource_ptr resource) & noexcept -> IO& {
    request_.resource = std::move(resource);
    return *static_cast<IO*>(this);
  }

  /** @brief 显式指定请求上下文；已经绑定资源的归属仍按资源协议校验。 */
  auto with_context(io_context context) && noexcept -> IO {
    context_ = std::move(context);
    return std::move(*static_cast<IO*>(this));
  }

  /** @brief 显式指定请求上下文；已经绑定资源的归属仍按资源协议校验。 */
  auto with_context(io_context context) & noexcept -> IO& {
    context_ = std::move(context);
    return *static_cast<IO*>(this);
  }

 protected:
  Request request_; /**< 默认策略保留完整请求；Recv/Send 的描述更紧凑，并自行持有参数。 */

 private:
  /** @brief 协作取消回调。 */
  struct cancel_callback {
    std::shared_ptr<io_domain>
        domain;            /**< 共享持有所属 I/O 域，使其在取消回调和完成发布期间保持有效。 */
    operation_token token; /**< 包含槽位和代际的操作身份，防止取消或完成事件命中过期操作。 */

    void operator()() const noexcept { domain->request_cancel(token); }
  };

  io_context context_;     /**< 持有所属 I/O 上下文租约，用于资源归属、服务选择与安全排空。 */
  bool cancellable_{true}; /**< 是否订阅任务取消；必须排空的清理请求可显式关闭取消。 */
  std::shared_ptr<io_domain> domain_; /**< 取消回调不借用运行时 worker 的地址。 */
  operation_token token_{}; /**< 包含槽位和代际的操作身份，防止取消或完成事件命中过期操作。 */
  std::optional<std::stop_callback<cancel_callback>>
      stop_callback_; /**< 按需就地保存的停止回调，析构时注销并等待在途回调结束。 */
};

}  // namespace faio::io::detail
