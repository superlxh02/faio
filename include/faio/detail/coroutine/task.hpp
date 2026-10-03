#ifndef FAIO_DETAIL_COROUTINE_TASK_HPP
#define FAIO_DETAIL_COROUTINE_TASK_HPP

#include "faio/detail/coroutine/frame_allocator.hpp"
#include "faio/detail/coroutine/task_context.hpp"
#include "faio/detail/coroutine/this_coro.hpp"
#include <concepts>
#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace faio {
// 前置声明用户任务类型，供 promise 的返回对象声明使用。
template <class T>
class task;

namespace detail {
// 把协程表达式转换成 awaiter。普通 awaitable 走原生协议；task 的
// operator co_await 在这里取得独占所有权。此处只在编译期分派，不分配内存。
template <class A>
decltype(auto) as_awaiter(A&& value) {
  // 编译期优先选择成员 operator co_await，保留表达式的左/右值属性。
  if constexpr (requires { std::forward<A>(value).operator co_await(); }) {
    // 让 task 等类型把帧所有权转给其 awaiter。
    return std::forward<A>(value).operator co_await();
    // 没有成员转换时尝试 ADL 找到的非成员 operator co_await。
  } else if constexpr (requires { operator co_await(std::forward<A>(value)); }) {
    // 调用自定义非成员转换，保持原生 C++ awaitable 协议。
    return operator co_await(std::forward<A>(value));
  } else {
    // 没有转换函数时，表达式本身就是 awaiter，直接保留它。
    return std::forward<A>(value);
  }
}

// 给任意等待操作加一层任务上下文恢复适配，不引入堆分配。
// inner 可以是借用的左值 awaiter，也可以是由本对象拥有的右值 awaiter。
template <class A>
struct scoped_awaiter {
  // 实际等待协议对象，await_ready/await_suspend/await_resume 都转发给它。
  A inner;
  // 借用所属 promise 的上下文；父帧在本次等待结束前始终存活且地址稳定。
  // 不复制 stop_token，避免每个 co_await 增减停止状态的原子引用计数。
  task_context* context;

  // 挂起前检查：保持底层等待操作的立即完成判定。
  decltype(auto) await_ready() { return inner.await_ready(); }

  // 挂起时：保持底层 await_suspend 的 bool/void/协程句柄返回语义。
  template <class H>
  decltype(auto) await_suspend(H h) {
    // 不改变底层选择的挂起方式或对称转移目标。
    return inner.await_suspend(h);
  }

  // 恢复时：先重建本任务 TLS，再领取底层结果或让底层抛出异常。
  decltype(auto) await_resume() {
    // I/O、定时器和同步原语可能迁移；借用的 promise 上下文仍稳定。
    // 在用户语句执行前完整恢复；标准库会跳过相同停止状态的复制赋值。
    restore_task_context(*context);
    // 即使底层恢复抛异常，异常处理中的 spawn 归属也已经正确。
    return inner.await_resume();
  }
};

// 有结果和 void 任务共用的 promise：保存续体、异常及执行属性。
struct task_promise_base {
  // 本任务结束后接续执行的父协程句柄，不拥有父帧。
  std::coroutine_handle<> continuation{};
  // 离开协程体的未处理异常，延迟到父任务领取结果时重抛。
  std::exception_ptr exception{};
  // 此任务随帧保存的调度、任务组、停止、优先级与协作预算。
  task_context context{};

  // 等待表达式转换时：将停止令牌查询标记转换为立即完成的 awaiter。
  auto await_transform(this_coro::stop_token_t) noexcept {
    // 查询的是 promise 保存的令牌，不是此时线程上可能变化的 TLS。
    return query_awaiter<std::stop_token>{context.stop_token};
  }

  // 等待表达式转换时：读取本任务所属的借用调度句柄。
  auto await_transform(this_coro::scheduler_t) noexcept {
    // 立即返回小句柄，不启动调度、不延长运行时生命周期。
    return query_awaiter<scheduler_ref>{context.scheduler};
  }

  // 等待表达式转换时：创建动态 worker 查询器，恢复时读取所在 worker。
  auto await_transform(this_coro::worker_id_t) noexcept { return worker_id_awaiter{}; }

  // 等待表达式转换时：查询当前任务的优先级元数据。
  auto await_transform(this_coro::priority_t) noexcept {
    // 立即返回优先级，不代表当前调度器实现了抢占或优先队列。
    return query_awaiter<task_priority>{context.priority};
  }

  // 等待表达式转换时：构造会主动重新排队的让出操作。
  auto await_transform(this_coro::yield_t) noexcept {
    // 将恢复需要的调度器、任务组和令牌一并保存在 awaiter 中。
    return yield_awaiter{context.scheduler, context.scope, &context.stop_token};
  }

  // 等待表达式转换时：构造使用本轮实际恢复共享预算的条件让出操作。
  auto await_transform(this_coro::yield_if_needed_t) noexcept {
    // 上下文仅用于调度/停止恢复与预算镜像；真实剩余额度由共享 TLS 保存。
    return yield_if_needed_awaiter{&context};
  }

  // 等待表达式转换时：保留原生 awaitable，并添加任务上下文恢复适配。
  template <class A>
  auto await_transform(A&& value) {
    // 只推导实际 awaiter 类型，decltype 不执行转换或创建对象。
    using awaiter_t = decltype(as_awaiter(std::forward<A>(value)));
    // 左值 awaiter 保留引用；右值 awaiter 按值保存，避免借用临时对象。
    using stored_t = std::conditional_t<std::is_lvalue_reference_v<awaiter_t>,
                                        awaiter_t,
                                        std::remove_cvref_t<awaiter_t>>;
    // 此处才真正取得 awaiter，连同恢复上下文一起交给编译器协程协议。
    return scoped_awaiter<stored_t>{as_awaiter(std::forward<A>(value)), &context};
  }

  // 首次挂起时：保持惰性，创建 task 后不立即执行协程函数体。
  std::suspend_always initial_suspend() const noexcept { return {}; }

  // 无成员的最终挂起 awaiter，以对称转移把执行权直接交回父协程。
  struct final_awaiter {
    // 挂起前检查：任务结束时总进入最终挂起，等待持有者安全销毁子帧。
    bool await_ready() const noexcept { return false; }

    template <class Promise>
    // 最终挂起时：返回父续体句柄，避免递归调用 parent.resume()。
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> child) const noexcept {
      // 对称转移直接返回 continuation，避免深层 task 链递归 resume。
      // 读取启动任务时保存的父协程，不延长父协程生命周期。
      auto parent = child.promise().continuation;
      // 有父协程则对称转移；没有续体则交给空协程，不执行额外用户代码。
      return parent ? parent : std::noop_coroutine();
    }

    // 恢复时：最终挂起不会被正常恢复，此函数只补齐 awaiter 协议。
    void await_resume() const noexcept {}
  };

  // 最终挂起时：保留当前帧的结果，由父 awaiter 领取后销毁。
  final_awaiter final_suspend() const noexcept { return {}; }

  // 异常离开协程体时：保存异常，随后仍经过最终挂起回到父协程。
  void unhandled_exception() noexcept { exception = std::current_exception(); }

  // 结果消费辅助接口：有已记录异常时重抛，否则允许读取返回值。
  void rethrow_if_failed() const {
    // 失败优先于结果读取，避免访问尚未构造的 optional。
    if (exception)
      std::rethrow_exception(exception);
  }
};

// 有返回值任务的 promise，在共用状态外增加一个尚未构造的结果槽。
template <class T>
struct task_promise : task_promise_base, task_frame_allocation<task_promise<T>> {
  // 禁止直接返回引用类型，需要借用时由调用者显式选择 reference_wrapper 等类型。
  static_assert(!std::is_reference_v<T>, "task<T&> 会产生悬空引用；请返回值或引用包装器");
  // co_return 之前为空；正常返回时原位构造 T，父任务消费时移动取出。
  std::optional<T> value;

  // 创建协程时：从本 promise 构造移动独占的 task 返回对象。
  task<T> get_return_object() noexcept;

  template <class U>
    requires std::constructible_from<T, U&&>
  // 执行 co_return 时：将返回表达式转发到结果槽，保留移动语义。
  void return_value(U&& result) {
    value.emplace(std::forward<U>(result));
  }

  // 领取有返回值任务结果；必须在任务结束后调用，失败时先重抛异常。
  T take_result() {
    // 先确认协程正常返回，随后才访问结果槽。
    rethrow_if_failed();
    // 移动结果给父协程，task 的一次性所有权避免重复消费。
    return std::move(value.value());
  }
};

// 无返回值任务的 promise，只复用异常与上下文，不存放用户结果。
template <>
struct task_promise<void> : task_promise_base, task_frame_allocation<task_promise<void>> {
  // 创建协程时：从 void promise 构造对应 task<void>。
  task<void> get_return_object() noexcept;

  // 执行无值 co_return 或运行到函数末尾时：标记正常返回，无用户值需要存储。
  void return_void() noexcept {}

  // 领取 void 任务完成结果，仅检查并传播未处理异常。
  void take_result() { rethrow_if_failed(); }
};
}  // namespace detail

// 一次性惰性任务：创建时挂起，co_await 必须消费右值。
//  持有者销毁未启动的任务是安全的；已启动但仍挂起的任务不能被外部销毁，
//  因为 io_uring 及等待队列可能还持有帧内地址。完成后由 await_resume 销毁帧。
//  用户任务帧的移动独占持有者；等待或提交时必须转移所有权。
//  任务未启动时可以安全销毁，已启动且被 I/O/队列借用的帧必须等安全完成。
template <class T = void>
class [[nodiscard]] task {
 public:
  // 公开结果类型，scope 等组合器据此推导返回值。
  using value_type = T;
  // 编译器协程协议使用的 promise 类型，负责结果和最终挂起。
  using promise_type = detail::task_promise<T>;
  // 带具体 promise 类型的句柄，用于访问本任务帧状态。
  using handle_type = std::coroutine_handle<promise_type>;

  // 构造空任务，可作为移动后的空状态；空任务不能被有效提交。
  task() noexcept = default;

  // 接管新建协程帧的唯一所有权，不在这里执行协程。
  explicit task(handle_type h) noexcept : handle_(h) {}

  // 释放仍归本对象所有的帧；正常 await/spawn 后句柄已转出，不重复销毁。
  ~task() {
    if (handle_)
      handle_.destroy();
  }

  // 禁止复制同一个帧的所有权。
  task(const task&) = delete;

  // 禁止复制赋值，避免双重等待和双重销毁。
  task& operator=(const task&) = delete;

  // 接管帧并将源任务置空，移动过程不执行用户代码。
  task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  // 先释放旧的未转出帧，再接管源任务；本接口不承担取消已挂起任务的功能。
  task& operator=(task&& other) noexcept {
    // 自移动赋值时不释放自身仍持有的帧。
    if (this != &other) {
      // 清理被覆盖的旧帧，前提是没有队列或 I/O 正在借用其地址。
      if (handle_)
        handle_.destroy();
      // 把唯一帧句柄从源对象转移过来。
      handle_ = std::exchange(other.handle_, {});
    }
    // 返回已经完成所有权转移的任务对象。
    return *this;
  }

  // 父协程等待用户 task 时的帧所有者，生命周期覆盖整个子任务执行。
  struct awaiter {
    // 被等待子协程的唯一句柄；await_resume 之后被交换为空。
    handle_type callee;

    // 从 task 接管子帧，暂不启动它。
    explicit awaiter(handle_type h) noexcept : callee(h) {}

    // 禁止复制子帧所有权。
    awaiter(const awaiter&) = delete;

    // 登记前转移等待者，源等待者不再销毁子帧。
    awaiter(awaiter&& other) noexcept : callee(std::exchange(other.callee, {})) {}

    // 清理仍持有的帧；正常恢复领取结果后 callee 已置空。
    ~awaiter() {
      if (callee)
        callee.destroy();
    }

    // 挂起前检查：空任务直接进入恢复阶段报告错误；有效惰性任务需要启动。
    bool await_ready() const noexcept { return !callee; }

    template <class Parent>
    // 挂起时：登记父续体、继承上下文，并对称转移到子协程。
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) noexcept {
      // 记录子任务结束后需要接续的父协程句柄。
      callee.promise().continuation = parent;
      // 父 promise 有任务上下文时按值继承，避免依赖偶然的 TLS 状态。
      if constexpr (requires { parent.promise().context; }) {
        // 复制子任务显式指定的优先级，以免继承父上下文时丢失。
        const auto requested_priority = callee.promise().context.priority;
        // 区分子任务自行指定与默认继承的优先级。
        const bool explicit_priority = callee.promise().context.priority_explicit;
        // 复制调度器、任务组、停止令牌及其他任务属性。
        callee.promise().context = parent.promise().context;
        // 显式覆盖的优先级在继承后重新应用。
        if (explicit_priority) {
          // 恢复子任务自己要求的优先级元数据。
          callee.promise().context.priority = requested_priority;
          // 保持显式覆盖标志，继续等待更深层任务时能识别该设置。
          callee.promise().context.priority_explicit = true;
        }
      } else {
        // 根包装 promise 没有 context 时，从入口安装的 TLS 继承任务组。
        callee.promise().context.scope = ::faio::detail::current_tracker;
        // 从根包装安装的 TLS 继承停止令牌。
        callee.promise().context.stop_token = ::faio::detail::current_stop_token;
      }
      // 父上下文没有调度器时，才回退到当前 worker 登记的调度器。
      if (!callee.promise().context.scheduler)
        // 填入所属运行时的借用句柄，供后续让出或等待恢复使用。
        callee.promise().context.scheduler = ::faio::detail::current_scheduler();
      // 进入子协程前安装令牌，子协程的第一个等待操作即可观察停止。
      ::faio::detail::current_stop_token = callee.promise().context.stop_token;
      // 直接返回子句柄形成对称转移，父协程在子任务结束前保持挂起。
      return callee;
    }

    // 恢复时：领取子任务结果或重抛异常，并保证两条路径均销毁子帧。
    T await_resume() {
      // 把等待空任务变成明确错误，不解引用空句柄。
      if (!callee)
        throw std::logic_error("co_await 空 task");

      // 协作额度属于本次 scheduler resume 的共享 TLS；父子结果传递不重置预算。
      // 与 stdexec 一样，即使取值抛异常也要释放子协程帧。
      // 本次结果消费的局部帧清理守卫，结果移动或异常重抛都能清理子帧。
      struct frame_guard {
        // 从 awaiter 转入守卫的唯一子帧句柄。
        handle_type handle;

        // 离开 await_resume 时销毁子帧，包括异常展开路径。
        ~frame_guard() { handle.destroy(); }

        // 先转移销毁责任，再读结果，防止结果操作抛异常导致重复销毁。
      } guard{std::exchange(callee, {})};

      // void 任务只确认完成或重抛异常。
      if constexpr (std::is_void_v<T>)
        guard.handle.promise().take_result();
      // 有结果任务先取出值，随后局部守卫销毁子帧。
      else
        return guard.handle.promise().take_result();
    }
  };

  // 右值等待消费 task 所有权，awaiter 接管帧，原 task 变为空。
  awaiter operator co_await() && noexcept { return awaiter{std::exchange(handle_, {})}; }

  // 禁止隐式等待左值 task，调用者须 std::move 明确转交一次性所有权。
  awaiter operator co_await() & = delete;

  // 覆盖从父任务继承的优先级元数据；当前调度队列仍按 FIFO 运行。
  // 对尚未提交的右值任务设置显式优先级元数据，并把任务移动返回。
  task with_priority(task_priority priority) && noexcept {
    // 空任务没有 promise，保持为空而不访问它。
    if (handle_) {
      // 把指定优先级记录到任务帧。
      handle_.promise().context.priority = priority;
      // 防止后续从父任务继承上下文时覆盖这次显式设置。
      handle_.promise().context.priority_explicit = true;
    }
    // 返回仍拥有该帧的新任务对象，支持链式提交。
    return std::move(*this);
  }

  // 低层提交接口：取走唯一句柄；调用者此后承担执行和销毁责任。
  handle_type take() {
    // 拒绝从已转出或默认构造的空任务再次取帧。
    if (!handle_)
      throw std::logic_error("空 task 不能提交");
    // 转出唯一所有权，本对象析构时不再销毁该帧。
    return std::exchange(handle_, {});
  }

 private:
  // 尚未等待或提交的任务帧句柄，空值表示已转出或未绑定任务。
  handle_type handle_{};
};

// 创建协程时：由有结果 promise 产生其用户 task 对象。
template <class T>
inline task<T> detail::task_promise<T>::get_return_object() noexcept {
  // 从 promise 定位完整协程帧，把新帧所有权交给 task。
  return task<T>{std::coroutine_handle<task_promise<T>>::from_promise(*this)};
}

// 创建协程时：由 void promise 产生对应用户任务。
inline task<void> detail::task_promise<void>::get_return_object() noexcept {
  // 构造 void 帧句柄，任务仍会在 initial_suspend 保持惰性。
  return task<void>{std::coroutine_handle<task_promise<void>>::from_promise(*this)};
}
}  // namespace faio
#endif
