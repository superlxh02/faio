/**
 * @file faio/detail/execution/execute.hpp
 * @brief 将同步调用交给隔离线程执行的协程桥接：同步调用异步化等待器。
 * @details 同步工作由独立阻塞服务执行；有界队列以错误返回背压。停止仅记录取消原因，已开始的借用型调用须真实返回后才发布完成并恢复协程。
 */
#pragma once

#include "faio/detail/coroutine/task_context.hpp"
#include "faio/detail/execution/blocking_executor.hpp"
#include "faio/detail/io/core/context.hpp"
#include <atomic>
#include <chrono>
#include <concepts>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <variant>

namespace faio::execution {
namespace detail {
template <class T>
struct unwrap_expected {
  using type = T;  ///< 当前类型变换得到的目标类型。
  static constexpr bool wrapped = false;  ///< 输入结果是否已为 expected，用于避免嵌套错误包装。
};

template <class T>
struct unwrap_expected<expected<T>> {
  using type = T;  ///< 当前类型变换得到的目标类型。
  static constexpr bool wrapped = true;  ///< 输入结果是否已为 expected，用于避免嵌套错误包装。
};

/** @brief 任务稳定状态；池、等待者和 stop_callback 共享同一完成位置。 */
template <class F>
struct execute_state {
  using raw_result = std::invoke_result_t<F&>;  ///< 调用函数未经展开的原始返回类型。
  using result_type = typename unwrap_expected<raw_result>::type;  ///< 对外领取的结果类型，已去除内部 expected 包装。
  static_assert(!std::is_reference_v<result_type>, "阻塞服务不能返回借用引用");
  io::io_context context;  ///< 保持所属 IO 控制块和服务租约存活。
  blocking_executor_ref executor;  ///< 按请求选择文件、DNS或清理服务。
  bool cancellable{true};  ///< close等清理操作必须执行，不被父令牌跳过。
  F function;  ///< 提交前拥有完整系统调用参数，不借用临时 lambda。
  std::optional<expected<result_type>> result;  ///< 完成仲裁后写一次。
  scheduler_ref scheduler;  ///< 唤醒原任务的中立调度接口。
  std::atomic<unsigned char> handshake{0};  ///< 0 提交中，1 已挂起，2 已完成。
  std::mutex mutex;  ///< 只仲裁取消/结果，不跨系统调用持锁。
  std::optional<int> cancellation_reason;  ///< 第一个停止原因固定，后来的deadline不覆盖取消。
  bool terminal{};  ///< 结果发布以后，迟到取消不再修改结果。
  std::optional<std::chrono::steady_clock::time_point> deadline;  ///< 本次操作可选的单调时钟绝对截止时间。

  struct cancel_callback {
    execute_state* state;  ///< 关联操作的状态引用或指针，使用期间须由外部所有者保持存活。

    void operator()() const noexcept {
      /// @brief 以同一短锁串行化取消原因与完成终态，锁不跨越实际阻塞调用。
      std::lock_guard lock(state->mutex);
      /// @brief 仅在终态尚未建立且没有更早停止原因时记录本次取消。
      if (!state->terminal && !state->cancellation_reason)
        /// @brief 首次停止原因保持粘性，后到的取消或截止时间不会覆盖已确定原因。
        state->cancellation_reason = state->timed_out() ? ETIMEDOUT : ECANCELED;
    }
  };

  std::optional<std::stop_callback<cancel_callback>> stop_callback;  ///< 按需就地保存的停止回调，析构时注销并等待在途回调结束。
  std::optional<std::stop_callback<cancel_callback>> domain_stop_callback;  ///< 订阅 IO 域停止的回调；注销前域与操作状态均须有效。

  template <class Function>
  execute_state(io::io_context ctx,
                Function&& operation,
                blocking_executor_ref service,
                bool can_cancel)
      : context(std::move(ctx)),
        executor(std::move(service)),
        cancellable(can_cancel),
        function(std::forward<Function>(operation)) {}

  bool timed_out() const noexcept {
    /// @brief 只有可取消且设置截止时间的请求才进行单调时钟超时判定。
    return cancellable && deadline && std::chrono::steady_clock::now() >= *deadline;
  }

  /** @brief 在mutex保护下记录截止时间，保持先到的停止原因。 */
  void observe_deadline() noexcept {
    /// @brief 仅在尚无更早停止原因时把已到期请求标记为超时。
    if (!cancellation_reason && timed_out())
      /// @brief 固定首次停止原因，后来的父任务取消不会覆盖已记录的超时。
      cancellation_reason = ETIMEDOUT;
  }

  /** @brief 完成/取消在同一短锁中决定终态，恢复只经过保存的调度器。 */
  void complete(expected<result_type> value) noexcept {
    {
      /// @brief 以同一短锁串行化取消原因与完成结果，不把互斥锁持有期扩展到同步系统调用。
      std::lock_guard lock(mutex);
      /// @brief 被取消时保留已执行的字节进度，绝不在 syscall 排空前恢复借用者。
      observe_deadline();
      /// @brief 停止已先于终态确定时，将系统调用实际进度保留在取消或超时错误中。
      if (cancellation_reason) {
        /// @brief 未取得任何可报告传输时从零进度开始，随后按真实结果提取已有进度。
        std::size_t progress = 0;
        /// @brief 底层已返回错误时沿用其已完成进度，不能因外层取消而把进度清零。
        if (!value)
          /// @brief 从底层失败结果提取已经完成的字节进度。
          progress = value.error().progress();
        else if constexpr (std::integral<result_type>)
          /// @brief 整数型成功结果可表示已完成数量，将其保留在最终停止错误中。
          progress = static_cast<std::size_t>(*value);
        /// @brief 用首次停止原因替换可见终态，但保留真实执行已经产生的进度。
        value = std::unexpected{Error{*cancellation_reason, progress}};
      }
      /// @brief 在发布握手前完整构造结果槽，恢复方只能在观察完成状态后读取。
      result.emplace(std::move(value));
      /// @brief 在结果锁内固定唯一终态，迟到停止回调不再改写结果。
      terminal = true;
    }
    /// @brief 先 release 结果，再通过握手判断是否需要异步调度等待者。
    if (handshake.exchange(2, std::memory_order_acq_rel) == 1)
      /// @brief 将续体投递到捕获的调度器，发布后不得依赖等待器仍然存活。
      scheduler.schedule(continuation);
  }

  std::coroutine_handle<> continuation;  ///< 操作安全完成后应恢复的协程句柄，不负责销毁父协程。

  /// @brief 在独立阻塞线程上执行拥有的同步函数，开始前复查停止，返回后才发布完成。
  void run() noexcept {
    {
      /// @brief 以同一短锁串行化取消原因与完成结果，不把互斥锁持有期扩展到同步系统调用。
      std::lock_guard lock(mutex);
      /// @brief 排队期间停止可跳过 syscall；状态仍由已接受 job 完成。
      observe_deadline();
      /// @brief 排队期间已收到停止时跳过同步调用，仍为已接受任务建立一次失败终态。
      if (cancellation_reason) {
        /// @brief 排队时已经停止的任务跳过用户同步调用，仍由已接受工作发布一次失败完成。
        result.emplace(std::unexpected{make_error(*cancellation_reason)});
        /// @brief 在结果锁内固定唯一终态，迟到停止回调不再改写结果。
        terminal = true;
      }
    }
    /// @brief 开始执行前就已建立取消终态时，只完成挂起握手，不再调用用户函数。
    if (terminal) {
      /// @brief 发布完成并领取恢复责任；只有等待者已经挂起时才需要重新调度。
      if (handshake.exchange(2, std::memory_order_acq_rel) == 1)
        /// @brief 将续体投递到捕获的调度器，发布后不得依赖等待器仍然存活。
        scheduler.schedule(continuation);
      return;
    }
    try {
      /// @brief 底层本身返回 expected 时直接沿用其结果，避免再次套一层 expected。
      if constexpr (unwrap_expected<raw_result>::wrapped)
        /// @brief 执行拥有的同步调用，并把其已类型化的成功或错误交给统一完成仲裁。
        complete(std::invoke(function));
      else if constexpr (std::is_void_v<raw_result>) {
        /// @brief 在独立服务线程调用无返回值函数，异常仍由外层统一转换为失败结果。
        std::invoke(function);
        /// @brief 无返回值系统调用正常结束后，使用成功的 expected<void> 发布完成。
        complete(expected<void>{});
      } else
        /// @brief 将普通返回值包装为 expected 后进入与原生 expected 相同的完成发布路径。
        complete(expected<result_type>{std::invoke(function)});
    } catch (const std::filesystem::filesystem_error& error) {
      /// @brief 将文件系统异常中的系统错误码转换为异步失败结果。
      complete(std::unexpected{make_error(error.code().value())});
    } catch (const std::bad_alloc&) {
      /// @brief 将分配异常转为明确内存不足错误，仍履行已接受任务的完成责任。
      complete(std::unexpected{make_error(ENOMEM)});
    } catch (...) {
      /// @brief 将未分类异常转为 IO 错误，防止后台任务退出而等待者永远不恢复。
      complete(std::unexpected{make_error(EIO)});
    }
  }
};
}

/** @brief 类型化阻塞请求的协程桥接，构造时不提交、不执行系统调用。 */
template <class F>
class execute_awaiter {
 public:
  using state_type = detail::execute_state<F>;  ///< 本桥接对象使用的稳定共享状态类型。
  using result_type = typename state_type::result_type;  ///< 对外领取的结果类型，已去除内部 expected 包装。

  template <class Function>
    requires std::constructible_from<F, Function&&>
  execute_awaiter(io::io_context context,
                  Function&& function,
                  blocking_executor_ref executor,
                  bool cancellable = true)
      : state_(std::make_shared<state_type>(std::move(context),
                                            std::forward<Function>(function),
                                            std::move(executor),
                                            cancellable)) {}

  execute_awaiter(const execute_awaiter&) = delete;

  execute_awaiter& operator=(const execute_awaiter&) = delete;

  execute_awaiter(execute_awaiter&&) = default;

  /** @brief 设置相对超时；借用型操作等待已开始 syscall 排空后才返回。 */
  template <class Rep, class Period>
  execute_awaiter& set_timeout(std::chrono::duration<Rep, Period> duration) & {
    /// @brief 在提交前保存相对超时对应的单调时钟绝对截止时间。
    state_->deadline = std::chrono::steady_clock::now() + duration;
    return *this;
  }

  /// @brief 把相对时长转换为单调时钟截止时间并保存到本次等待。
  template <class Rep, class Period>
  execute_awaiter&& set_timeout(std::chrono::duration<Rep, Period> duration) && {
    set_timeout(duration);
    return std::move(*this);
  }

  /// @brief 保存单调时钟绝对截止时间，真正超时仲裁由提交与驱动协议完成。
  execute_awaiter& set_timeout_at(std::chrono::steady_clock::time_point deadline) & {
    /// @brief 保存绝对截止时间，本方法只更新参数，不创建定时线程或提交系统调用。
    state_->deadline = deadline;
    return *this;
  }

  /// @brief 保存单调时钟绝对截止时间，真正超时仲裁由提交与驱动协议完成。
  execute_awaiter&& set_timeout_at(std::chrono::steady_clock::time_point deadline) && {
    set_timeout_at(deadline);
    return std::move(*this);
  }

  /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
  bool await_ready() const noexcept { return false; }

  /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
  template <class Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    /// @brief 局部 lease 覆盖发布后立即在其他 worker 恢复和销毁 awaiter 的竞态。
    auto state = state_;
    /// @brief 在提交前记录恢复句柄，后台线程可能在提交调用返回前就完成。
    state->continuation = continuation;
    /// @brief 保存本次等待所继承的停止令牌，随后按 promise 或线程上下文选择来源。
    std::stop_token token;
    /// @brief 父 promise 提供任务上下文时直接继承其归属与停止令牌，否则读取根入口 TLS。
    if constexpr (requires { continuation.promise().context; }) {
      /// @brief 优先使用父协程明确保存的调度归属，避免受临时线程迁移影响。
      state->scheduler = continuation.promise().context.scheduler;
      /// @brief 继承父任务的协作取消状态，保持结构化任务组的停止传播。
      token = continuation.promise().context.stop_token;
    } else {
      /// @brief 没有任务上下文的根包装从当前线程执行绑定取得恢复调度器。
      state->scheduler = ::faio::detail::current_scheduler();
      /// @brief 从当前根任务入口安装的线程上下文捕获停止令牌。
      token = ::faio::detail::current_stop_token;
    }
    /// @brief 异步执行必须有合法恢复目标，否则在接纳工作之前报告调用错误。
    if (!state->scheduler)
      throw std::logic_error("阻塞 IO 必须由运行时调度");
    /// @brief callback 构造可同步触发；此时共享状态已经完整建立。
    if (state->cancellable) {
      /// @brief 不可请求停止的令牌无需注册回调，减少无效取消链操作。
      if (token.stop_possible())
        /// @brief 共享操作状态完整建立后再订阅父任务停止；构造可同步触发回调，仅记录首次停止原因。
        state->stop_callback.emplace(token, typename state_type::cancel_callback{state.get()});
      /// @brief 另取所属 IO 域的停止令牌，使运行时关闭也能标记本次阻塞请求取消。
      auto domain_token = state->context.stop_token();
      /// @brief 只有域提供有效停止源时才注册域级取消连接。
      if (domain_token.stop_possible())
        /// @brief 同时订阅域停机停止源，使运行时关闭也能标记排队或执行中的同步调用取消。
        state->domain_stop_callback.emplace(domain_token,
                                            typename state_type::cancel_callback{state.get()});
    }
    /// @brief 服务容量不足直接完成，调用者得到明确背压错误。
    if (!state->cancellable) {
      /// @brief 清理不占普通admission额度；饱和时由domain保留完成责任并重试。
      state->context.defer_cleanup([state] { state->run(); });
    } else {
      /// @brief 把共享状态租约移入已接受工作，使后台执行覆盖等待器提前完成或迁移的生命期竞争。
      auto accepted = state->executor.try_submit([state] { state->run(); });
      /// @brief 服务未接管工作时立即发布其背压或关闭错误，等待者不能继续悬挂。
      if (!accepted)
        /// @brief 把执行器拒绝转换为已构造等待操作的完成，握手会决定是否需要异步恢复。
        state->complete(std::unexpected{accepted.error()});
    }
    /// @brief 仅从提交中的初始状态竞争挂起资格；提前完成会把状态改为完成。
    unsigned char submitting = 0;
    /// @brief 先完成时返回 false，已挂起时由完成线程排队恢复，保证唯一唤醒。
    return state->handshake.compare_exchange_strong(submitting, 1, std::memory_order_acq_rel);
  }

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  expected<result_type> await_resume() {
    /// @brief 结果安全可取后注销任务级停止回调，并等待正在运行的回调结束。
    state_->stop_callback.reset();
    /// @brief 注销域级停止回调，确保返回后不再借用本次状态。
    state_->domain_stop_callback.reset();
    /// @brief 移动领取已经发布的 expected 结果，调用方据此处理返回值或错误。
    return std::move(state_->result.value());
  }

 private:
  std::shared_ptr<state_type> state_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
};

template <class F>
execute_awaiter(io::io_context, F&&, blocking_executor_ref, bool = true)
    -> execute_awaiter<std::decay_t<F>>;

/** @brief 提交受控系统调用到 context 的文件服务，自动展开 expected 返回值。 */
template <class F>
  requires std::invocable<std::decay_t<F>&>
auto execute(io::io_context context, F&& function) {
  /// @brief 取得所属 IO 上下文的文件阻塞服务，保留独立服务租约。
  auto service = context.blocking();
  return execute_awaiter<std::decay_t<F>>{
      std::move(context), std::forward<F>(function), std::move(service)};
}

/** @brief 显式选择阻塞服务，例如 resolver lane；参数及完成状态均拥有生命周期。
 */
template <class F>
  requires std::invocable<std::decay_t<F>&>
auto execute_blocking(io::io_context context, F&& function, blocking_executor_ref executor) {
  return execute_awaiter<std::decay_t<F>>{
      std::move(context), std::forward<F>(function), std::move(executor)};
}

/** @brief 不可取消的清理请求；只用于 close 等必须排空的受控系统调用。 */
template <class F>
  requires std::invocable<std::decay_t<F>&>
auto execute_cleanup(io::io_context context, F&& function) {
  /// @brief 选择专用清理服务，必须执行的关闭工作不被普通业务容量永久排斥。
  auto service = context.cleanup();
  return execute_awaiter<std::decay_t<F>>{
      std::move(context), std::forward<F>(function), std::move(service), false};
}
}
