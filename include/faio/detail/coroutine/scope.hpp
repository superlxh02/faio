#ifndef FAIO_DETAIL_COROUTINE_SCOPE_HPP
#define FAIO_DETAIL_COROUTINE_SCOPE_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/join_handle.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <type_traits>

namespace faio {
namespace detail {
// 结构化任务组的内部状态，存放于 scope 的协程帧。
// 所有子任务只借用该状态；scope 排空子任务后才允许销毁这块存储。
struct scope_state {
  // 初始化必须提供的两个借用入口；其他成员按各自默认值构造。
  scope_state(scheduler_ref target, task_tracker *outer) noexcept
      : scheduler(target), tracker(outer) {}
  // 借用 scope 所属调度器，用于提交每个子任务，不拥有运行时。
  scheduler_ref scheduler;
  // 借用外层任务计数器，例如 block_on 的任务组；与本 scope 的 remaining
  // 分开计数。
  ::faio::detail::task_tracker *tracker;
  // 独立借用根任务计数服务，与纯调度引用分开保存。
  task_lifetime_ref lifetime{current_task_lifetime()};
  // 整个 scope 的停止源，向所有通过 scope.spawn 提交的孩子传播停止请求。
  std::stop_source stop;
  // 未完成孩子数加一个 body 哨兵；body 退出时归还哨兵，避免提交途中提前完成。
  std::atomic<std::size_t> remaining{1};
  // remaining 归零时置位的一次性事件，供 scope 协程等待全部孩子退出。
  ::faio::detail::completion_event completion;
  // 保护多个子任务同时记录异常，保证 first_error 只被首次失败写入。
  std::mutex error_mutex;
  // 首次记录的 body/子任务异常；排空完成后向调用方重抛。
  std::exception_ptr first_error;

  // 记录首次异常，并在退出错误锁后请求停止兄弟任务。
  // 停止回调可能获取其他原语的锁，因此不在 error_mutex 临界区内执行它。
  void fail(std::exception_ptr error) {
    {
      // 串行化异常记录，防止并发写入同一个 exception_ptr。
      std::lock_guard lock(error_mutex);
      // 保留首次失败原因，后续取消或异常不会覆盖它。
      if (!first_error)
        first_error = std::move(error);
    }
    // 向孩子发协作停止请求；这里不直接销毁孩子的协程帧。
    stop.request_stop();
  }
  // 归还一个孩子或 body 的计数；最后一个完成者发布整组完成事件。
  void done() {
    // 递减并汇合各完成者的写入；旧值为 1 表示这次递减后整组归零。
    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
      // 发布完成状态，使父协程可安全读取结果、异常并销毁 scope_state。
      completion.notify();
  }
};

// 将用户孩子包装成调度器拥有的根协程。
// child 的所有权转入根帧；state 由正在等待排空的 scope 帧保证存活。
template <class T>
detached_task scope_child(task<T> child, scope_state *state) {
  // 建立外层任务组，使孩子继续派生的根任务登记到正确 tracker。
  ::faio::detail::current_tracker = state->tracker;
  // 安装本 scope 的停止令牌，用户 task 在启动时继承它。
  ::faio::detail::current_stop_token = state->stop.get_token();
  // 消费并等待孩子完成；子任务结果在此丢弃，scope.spawn 不返回单个孩子结果。
  try {
    co_await std::move(child);
  }
  // 区分兄弟失败导致的正常停止与未预期的独立取消。
  catch (const operation_cancelled &) {
    // scope 已发停止时不把取消当作新错误；否则将该取消作为本组失败记录。
    if (!state->stop.stop_requested())
      state->fail(std::current_exception());
  }
  // 其他异常成为 scope 的失败，并触发兄弟停止。
  catch (...) {
    state->fail(std::current_exception());
  }
  // 无论成功或已捕获的失败，都归还孩子计数；此后不再访问 state。
  state->done();
}
} // namespace detail

// 提供给 body 的子任务组操作视图，只借用内部状态。
// 仅通过此对象 spawn 的孩子参与 scope 自身的完成计数。
class scope_context {
public:
  // 绑定正在执行的 scope 状态；引用不得逃逸到 scope 已完成之后。
  explicit scope_context(detail::scope_state &state) noexcept
      : state_(&state) {}
  // 禁止复制操作视图，避免无意扩散这个受作用域限制的借用。
  scope_context(const scope_context &) = delete;
  // 禁止复制赋值，视图始终对应构造时的 scope。
  scope_context &operator=(const scope_context &) = delete;

  // 子任务必须在 scope 结束前完成。失败会请求停止所有兄弟任务，
  // scope 等它们退出后才传播第一个异常，不留下引用已失效的后台任务。
  // 立即提交一个受本 scope 管理的孩子，消费 child 的帧所有权。
  // 提交失败会归还本组计数，并把错误抛给 body。
  template <class T> void spawn(task<T> child) {
    // 先创建惰性根包装；创建失败时尚未增加计数。
    auto root = detail::scope_child(std::move(child), state_);
    // 投递之前登记孩子，防止孩子执行过快导致计数遗漏。
    state_->remaining.fetch_add(1, std::memory_order_relaxed);
    // 同时登记外层任务组，根 promise 的析构负责归还外层计数。
    if (state_->tracker)
      state_->tracker->add();
    try {
      // 将根帧交给调度器；正常结束或提交失败时由根帧析构处理运行时登记。
      start_detached(std::move(root), state_->scheduler, state_->tracker,
                     state_->lifetime);
    } catch (...) {
      // 根帧未执行协程体，需在失败分支手动归还本 scope 的孩子计数。
      state_->done();
      // 将提交错误交给 body，外层 scope 会记录失败并等待其他孩子退出。
      throw;
    }
  }
  // 主动请求停止本组孩子；返回不代表孩子已经结束，scope 仍会排空。
  void request_stop() noexcept { state_->stop.request_stop(); }
  // 取得本组停止令牌，允许 body 的其他操作显式观察停止状态。
  std::stop_token stop_token() const noexcept {
    return state_->stop.get_token();
  }

private:
  // 借用 scope 帧中的状态，不分配内存，也不延长其生命周期。
  detail::scope_state *state_;
};

// body(scope_context&) 返回 task<T>。body 返回后仍等待其启动的所有子任务；
// body 或任一子任务抛异常时请求停止，等待清理完成后向调用者传播异常。
// 构造惰性的结构化作用域任务；F 按值保存在 scope 帧中。
// 只有 co_await/block_on 启动它后才调用 body(context)，排空后返回 body 的结果。
template <class F>
auto scope(F body)
    -> task<typename std::invoke_result_t<F &, scope_context &>::value_type> {
  // 从 body 返回的 task 中推导结果类型，支持 void。
  using T = typename std::invoke_result_t<F &, scope_context &>::value_type;
  // 查询本作用域的调度器，所有孩子均提交到该运行时。
  auto scheduler = co_await this_coro::scheduler();
  // 查询外层停止令牌，后续转发到本组独立的停止源。
  auto parent_stop = co_await this_coro::stop_token();
  // 在 scope 帧内构造共享给孩子的状态，初始 remaining 含 body 哨兵。
  detail::scope_state state{scheduler, ::faio::detail::current_tracker};
  // 帧内保存父停止回调；它存活期间维持父停止源到本组停止源的连接。
  std::optional<std::stop_callback<detail::forward_stop>> parent_callback;
  // 不可停止的父任务无需登记回调，减少无取消场景的额外工作。
  if (parent_stop.stop_possible())
    // 父令牌已停止时也会同步转发，孩子启动后可立即观察请求。
    parent_callback.emplace(parent_stop, detail::forward_stop{&state.stop});
  // 创建供 body 使用的视图，借用同一个 scope_state。
  scope_context context{state};
  // 暂存 body 结果；void 使用 monostate，使两种路径共用结果存在性标记。
  std::optional<detail::handle_value<T>> result;
  try {
    // body 与孩子使用同一 scope 停止源。独立根包装负责继承该 token，
    // 否则 body 会只继承父任务 token，孩子失败无法取消正在等待的 body。
    auto body_task = std::invoke(body, context);
    join_handle<T> body_handle{
        detail::start_observed(scheduler, std::move(body_task), state.tracker,
                               state.stop.get_token(), state.lifetime)};
    if constexpr (std::is_void_v<T>) {
      // JoinHandle 即使已请求停止仍等到 body 真实结束，保持 context 借用安全。
      co_await body_handle;
      // 记录 body 正常完成，无需保存用户值。
      result.emplace();
    } else {
      // 启动有结果的 body 并暂存结果，先不向调用者返回。
      result.emplace(co_await body_handle);
    }
    // body 执行、返回值构造或提交孩子失败时记录异常并请求停止。
  } catch (...) {
    state.fail(std::current_exception());
  }
  // body 已退出，归还启动哨兵，让最后一个孩子有权发布完成。
  state.done();
  // 挂起等待 remaining 归零；即使本 scope 已停止，也必须等孩子清理完毕。
  co_await state.completion.wait();
  // 所有借用者都已退出，现在可以安全向父协程传播首个异常。
  if (state.first_error)
    std::rethrow_exception(state.first_error);
  // 无结果作用域在排空后正常完成。
  if constexpr (std::is_void_v<T>)
    co_return;
  // 有结果作用域在排空后把 body 结果移动给调用方。
  else
    co_return std::move(*result);
}
} // namespace faio
#endif
