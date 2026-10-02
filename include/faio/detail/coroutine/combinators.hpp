#ifndef FAIO_DETAIL_COROUTINE_COMBINATORS_HPP
#define FAIO_DETAIL_COROUTINE_COMBINATORS_HPP

#include "faio/detail/coroutine/root_task.hpp"
#include "faio/detail/coroutine/coroutine_wait.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace faio {
namespace detail {
// 统一组合结果：void 分支以 monostate 占位，保留输入参数的下标对应关系。
// 单个 join 分支的结果槽，放在父组合任务帧的 tuple/vector 中。
// 每个槽仅由对应孩子写入，父协程等待全部完成后才读取。
template <class T> using joined_value = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
template <class T> struct join_slot {
  // 成功分支的结果，void 分支存占位；失败时保持为空。
  std::optional<joined_value<T>> value;
  // 本分支失败原因，父协程按输入下标取槽时重抛。
  std::exception_ptr error;
  // 消费已完成槽位的结果或重抛异常，调用前必须等待全部分支完成。
  joined_value<T> take() {
    // 先检查失败，避免读取失败分支未构造的结果。
    if (error) std::rethrow_exception(error);
    // 按输入顺序移动结果；组合任务只有一个结果消费者。
    return std::move(*value);
  }
};

// 单个 join 分支的根包装：拥有 child，借用父帧的槽位、计数和完成事件。
// 父组合器无论成功还是启动失败，都必须排空已提交根任务再离开。
template <class T>
detached_task join_child(task<T> child, join_slot<T>* slot,
                                           std::atomic<std::size_t>* remaining,
                                           ::faio::detail::completion_event* completion,
                                           ::faio::detail::task_tracker* scope,
                                           std::stop_token token) {
  // 安装外层任务组，分支继续 spawn 时沿用正确的计数归属。
  ::faio::detail::current_tracker = scope;
  // 安装调用方传入的停止令牌；join 不建立兄弟失败自动停止源。
  ::faio::detail::current_stop_token = token;
  try {
    // 编译期区分 void 完成占位与有值完成路径。
    if constexpr (std::is_void_v<T>) {
      // 消费并执行无结果孩子，异常交由本包装捕获。
      co_await std::move(child);
      // void 孩子正常完成，保存 monostate 占位。
      slot->value.emplace();
    } else {
      // 等待有值孩子并写入只属于它的结果槽。
      slot->value.emplace(co_await std::move(child));
    }
  // 记录孩子异常或结果存储异常，不让根任务的未处理异常终止进程。
  } catch (...) { slot->error = std::current_exception(); }
  // acq_rel 把每个槽位的写入发布给最后一个完成者；event 再发布给父协程。
  // 归还分支计数；最后一位通过事件发布所有槽位写入，此后不再访问父状态。
  if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) completion->notify();
}

// 归还启动哨兵或未执行分支的计数，复用最后完成者通知规则。
inline void finish_join_child(std::atomic<std::size_t>& remaining,
                              ::faio::detail::completion_event& event) {
  // 归零才发布完成，确保父协程可安全销毁所有借用状态。
  if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) event.notify();
}

// 构造并立即投递一个 join 孩子，提交失败时配对归还组合器计数。
template <class T>
void launch_join_child(task<T> child, join_slot<T>& slot,
                       std::atomic<std::size_t>& remaining,
                       ::faio::detail::completion_event& event) {
  // 保存提交瞬间的外层 tracker，根帧及孩子都沿用它。
  auto* scope = ::faio::detail::current_tracker;
  // 先构造包装帧，借用槽位地址；分配失败时尚未增加任何计数。
  auto root = join_child(std::move(child), &slot, &remaining, &event, scope,
                         ::faio::detail::current_stop_token);
  // 投递前登记孩子，孩子在其他 worker 立即完成也不会漏计。
  remaining.fetch_add(1, std::memory_order_relaxed);
  // 登记外层任务组，根 promise 析构负责减回这一个计数。
  if (scope) scope->add();
  try {
    // 提交根任务；所有运行时/外层计数的回收由根帧清理负责。
    start_detached(std::move(root), ::faio::detail::current_scheduler(), scope);
  } catch (...) {
    // 根协程尚未执行；其协程体里的减计数不会发生。
    // 协程体未执行，手动归还属于 join 的分支计数。
    finish_join_child(remaining, event);
    // 继续向组合器抛启动错误，组合器先排空已有分支再传播。
    throw;
  }
}

// 用编译期下标把异构输入任务逐个配对到同位置槽位并提交。
template <class... Ts, std::size_t... Is>
void start_join(std::tuple<join_slot<Ts>...>& slots,
                std::atomic<std::size_t>& remaining,
                ::faio::detail::completion_event& event,
                std::index_sequence<Is...>, task<Ts>... children) {
  // 逗号折叠按参数顺序执行提交，所有权逐个移交给根包装。
  (launch_join_child(std::move(children), std::get<Is>(slots), remaining, event), ...);
}
// 全部完成后按输入位置收集异构结果，第一个失败槽位在此重抛。
template <class... Ts, std::size_t... Is>
auto collect_join(std::tuple<join_slot<Ts>...>& slots, std::index_sequence<Is...>) {
  // 生成结果 tuple，完成顺序不会改变结果排列。
  return std::tuple<joined_value<Ts>...>{std::get<Is>(slots).take()...};
}

// 选择状态位于父 select 帧；返回与异常传播之前排空所有已提交分支。
template <class... Ts> struct select_state {
  // 异构结果载体，使用 variant 下标区分相同结果类型的不同分支。
  using variant_type = std::variant<joined_value<Ts>...>;
  // 胜出权仲裁位，只允许一个分支 CAS 成功；不是结果已经发布的标志。
  std::atomic<bool> claimed{false};
  // 胜出结果已经写好时的事件，供父选择器等待唯一赢家。
  ::faio::detail::completion_event completion;
  // 胜出结果，只有 CAS 获胜者写入，其他分支不覆盖。
  std::optional<variant_type> value;
  // 胜出分支的异常，选择器排空后重抛；失败分支也能取得胜出权。
  std::exception_ptr error;
  // 胜出分支在输入参数中的下标，仅 CAS 获胜者写入。
  std::size_t index{};
  // 启动阶段用一个哨兵保护计数。若中途失败，父协程等所有已启动分支退出。
  // 未退出分支数加一个启动哨兵，保护还在提交其余孩子的阶段。
  std::atomic<std::size_t> running{1};
  // 所有已启动分支退出事件，确保停止后真正排空再返回。
  ::faio::detail::completion_event all_done;
  // 候选任务共用停止源，赢家或外层停止回调向它发请求。
  std::stop_source stop;
  // 归还分支或启动哨兵计数，最后一位唤醒排空等待者。
  void child_done() {
    // 汇合分支完成并发布排空，父协程从此可以返回和释放借用对象。
    if (running.fetch_sub(1, std::memory_order_acq_rel) == 1) all_done.notify();
  }
};

// 第 I 个选择分支的根包装：先执行孩子，再以完成结果竞争唯一胜出权。
template <std::size_t I, class T, class State>
detached_task select_child(task<T> child,
                                             State* state,
                                             ::faio::detail::task_tracker* scope) {
  // 安装外层任务组，分支继续 spawn 时沿用正确的计数归属。
  ::faio::detail::current_tracker = scope;
  // 候选任务继承本次选择的停止令牌，获胜后其他孩子可响应取消。
  ::faio::detail::current_stop_token = state->stop.get_token();
  // 复用共享状态的 variant 类型，使本分支结果能按 I 写入对应备选项。
  using variant_type = typename State::variant_type;
  // 分支私有结果暂存区，仲裁前不写共享的赢家结果。
  std::optional<variant_type> value;
  // 当前分支暂存的异常，仅赢得选择后写入共享状态。
  std::exception_ptr error;
  try {
    // 编译期区分 void 完成占位与有值完成路径。
    if constexpr (std::is_void_v<T>) {
      // 消费并执行无结果孩子，异常交由本包装捕获。
      co_await std::move(child);
      // 无结果分支按原输入下标构造 monostate。
      value.emplace(std::in_place_index<I>);
    } else {
      // 消费有结果分支并按输入下标保存；即使结果类型重复也不会歧义。
      value.emplace(std::in_place_index<I>, co_await std::move(child));
    }
  // 失败同样代表完成，可以竞争成为本次选择的赢家。
  } catch (...) { error = std::current_exception(); }
  // CAS 仅在尚无人胜出时成功。
  bool expected = false;
  // 唯一成功者负责发布赢家结果；claimed 先置位，父协程仍须等 completion。
  if (state->claimed.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel)) {
    // 记录获胜的原始参数下标。
    state->index = I;
    // 把本分支的失败原因写入赢家状态；无异常时为空。
    state->error = std::move(error);
    // 值类型的移动可能抛异常；获胜权已经确定，必须仍然通知等待者。
    // 仅正常完成才转移结果，失败路径无需读取空 optional。
    if (!state->error) {
      // 移动私有结果到共享赢家槽；用户值的移动也可能失败。
      try { state->value.emplace(std::move(*value)); }
      // 结果移动失败仍记录错误并继续发布，避免 claimed 已置位却永远不通知。
      catch (...) { state->error = std::current_exception(); }
    }
    // 发布赢家值/异常，父协程能够观察完整结果。
    state->completion.notify();
    // 向落选分支发协作停止请求；已经完成的分支不回滚结果或副作用。
    state->stop.request_stop();
  }
  // 先销毁分支私有值/异常，用户析构也属于需要排空的分支清理。
  value.reset();
  error = {};
  // 最后归还完成计数；notify 后父帧可销毁，从此不再访问 state。
  state->child_done();
}
// 构造并立即提交第 I 个候选分支，只借用父帧状态；父任务排空后才能退出。
template <std::size_t I, class T, class State>
void launch_select_child(task<T> child, State* state) {
  // 保存提交瞬间的外层 tracker，根帧及孩子都沿用它。
  auto* scope = ::faio::detail::current_tracker;
  // 创建尚未启动的候选包装帧，转移用户 task 所有权。
  auto root = select_child<I>(std::move(child), state, scope);
  // 投递前登记候选，防止分支快速完成导致 running 提前归零。
  state->running.fetch_add(1, std::memory_order_relaxed);
  // 登记外层任务组，根 promise 析构负责减回这一个计数。
  if (scope) scope->add();
  try {
    // 提交根任务；所有运行时/外层计数的回收由根帧清理负责。
    start_detached(std::move(root), ::faio::detail::current_scheduler(), scope);
  } catch (...) {
    // 根协程未执行时归还预登记计数，防止排空永久等待。
    state->child_done();
    // 继续向组合器抛启动错误，组合器先排空已有分支再传播。
    throw;
  }
}
// 按轮转起点提交异构候选，保持结果下标仍对应原始参数位置。
template <class... Ts, std::size_t... Is>
void start_select(select_state<Ts...>* state,
                  std::index_sequence<Is...>, task<Ts>... children) {
  // 把输入帧所有权移入本地 tuple，按运行期轮转下标选择编译期元素。
  auto tasks = std::tuple<task<Ts>...>{std::move(children)...};
  // 轮转提交顺序使多个立即完成的分支长期运行时没有固定的 branch-0 偏置。
  // 每个 worker 的轮转游标，每种模板实例各自保存，减少固定第 0 分支偏置。
  static thread_local std::size_t next_start{};
  // 本次选择的首个提交下标，接口约束保证候选数不为 0。
  const auto start = next_start++ % sizeof...(Ts);
  // 循环每个候选恰好一次，不因提交顺序改变输入索引。
  for (std::size_t step = 0; step < sizeof...(Ts); ++step) {
    // 在环形下标空间计算本轮需要启动的候选。
    const auto index = (start + step) % sizeof...(Ts);
    // 用编译期折叠匹配运行期 index，仅命中的元素转交一次所有权。
    ((index == Is ? (launch_select_child<Is>(std::move(std::get<Is>(tasks)), state), void())
                  : void()), ...);
  }
}
// 外层停止源到本次选择停止源的回调连接。
struct forward_select_stop {
  // 借用 select_state 的停止源，回调注销前共享状态必须仍存活。
  std::stop_source* source;
  // 外层停止时请求全部候选停止，父选择器仍会等待它们排空。
  void operator()() const noexcept { source->request_stop(); }
};
} // namespace detail

// 函数调用只构造惰性 task；被 co_await 或 block_on 执行时才同时启动分支。
// 等全部完成后返回 tuple；void 用 std::monostate 占位，异常在所有分支
// 结束后按参数位置选择第一个并重新抛出。
// 惰性异构并发组合，执行时提交所有孩子并等待全部完成。
// 失败也等待全部退出；不因某个孩子失败自动请求兄弟停止。
template <class... Ts>
task<std::tuple<detail::joined_value<Ts>...>> join(task<Ts>... children) {
  // 空输入直接返回空 tuple，不构造等待设施。
  if constexpr (sizeof...(Ts) == 0) co_return {};
  else {
    // 结果槽放在父任务帧中，各孩子只借用各自槽位的地址。
    std::tuple<detail::join_slot<Ts>...> slots;
    // 初始 1 是启动哨兵，避免孩子在其他分支尚未提交时提前通知。
    std::atomic<std::size_t> remaining{1};
    // join 全部分支退出事件，父协程等它置位后才读取 tuple 槽位。
    ::faio::detail::completion_event completion;
    // 区别启动失败与用户孩子异常；启动失败先排空再优先重抛。
    std::exception_ptr launch_error;
    try {
      // 逐个提交异构输入；中途失败时已经启动的根协程仍由计数保护。
      detail::start_join(slots, remaining, completion, std::index_sequence_for<Ts...>{},
                         std::move(children)...);
    // 先保存启动错误，不能立即离开并销毁仍被孩子借用的槽位。
    } catch (...) { launch_error = std::current_exception(); }
    // 提交阶段结束，归还启动哨兵；没有孩子时也能正确归零。
    detail::finish_join_child(remaining, completion);
    // 等待全部已登记孩子退出，完成事件不因停止请求提前放行。
    co_await completion.wait();
    // 已有孩子全部结束后，才向调用方传播提交失败。
    if (launch_error) std::rethrow_exception(launch_error);
    // 按输入顺序取结果；槽位异常在父协程里重抛并由 task 传播。
    co_return detail::collect_join(slots, std::index_sequence_for<Ts...>{});
  }
}

// 同类型动态任务集合；同样是惰性 task，空集合执行后返回空 vector。
// 惰性同类动态任务组合，返回与输入数量和顺序对应的 vector。
template <class T>
task<std::vector<detail::joined_value<T>>> join_all(std::vector<task<T>> children) {
  // 最终返回容器，空输入时无需分配结果元素。
  std::vector<detail::joined_value<T>> result;
  // 空集合立即返回空 vector，无根任务提交。
  if (children.empty()) co_return result;
  // 一次确定槽位数量，提交后不扩容，保持孩子借用的槽位地址稳定。
  std::vector<detail::join_slot<T>> slots(children.size());
  // 初始 1 是启动哨兵，避免孩子在其他分支尚未提交时提前通知。
  std::atomic<std::size_t> remaining{1};
  // join_all 全部分支退出事件，保护 slots vector 的生命周期。
  ::faio::detail::completion_event completion;
  // 区别启动失败与用户孩子异常；启动失败先排空再优先重抛。
  std::exception_ptr launch_error;
  try {
    // 按输入顺序给每个同类孩子提交一份根包装。
    for (std::size_t i = 0; i < children.size(); ++i)
      // 将第 i 个 task 配对到第 i 个槽，不再保留输入 task 的帧所有权。
      detail::launch_join_child(std::move(children[i]), slots[i], remaining, completion);
  // 先保存启动错误，不能立即离开并销毁仍被孩子借用的槽位。
  } catch (...) { launch_error = std::current_exception(); }
  // 提交阶段结束，归还启动哨兵；没有孩子时也能正确归零。
  detail::finish_join_child(remaining, completion);
  // 等待全部已登记孩子退出，完成事件不因停止请求提前放行。
  co_await completion.wait();
  // 已有孩子全部结束后，才向调用方传播提交失败。
  if (launch_error) std::rethrow_exception(launch_error);
  // 排空后预留全部结果空间，避免结果收集时多次扩容。
  result.reserve(slots.size());
  // 按槽位顺序消费结果，遇到第一个失败槽位时重抛。
  for (auto& slot : slots) result.push_back(slot.take());
  // 把已完成的结果 vector 返回给唯一调用者。
  co_return result;
}

// 选择器对用户返回的结果，只有赢家的下标与值，没有落选任务的句柄。
template <class... Ts> struct select_result {
  // 赢家对应原输入参数位置；读取 value 时应使用同一 variant 下标。
  std::size_t index;
  // 赢家结果，void 以 monostate 占位；相同类型分支仍由下标区分。
  std::variant<detail::joined_value<Ts>...> value;
};

// 执行时返回最先完成的分支。获胜后请求停止其他分支，并在全部分支退出后返回，
// 从而保证外部引用和等待节点不会超过 select 调用者的生命周期。
// 惰性选择组合：启动候选，等待首个完成，再请求停止并排空全部候选。
// 首个完成也可能是异常；该异常在排空后向调用者重抛。
template <class... Ts> requires (sizeof...(Ts) > 0)
task<select_result<Ts...>> select(task<Ts>... children) {
  // 状态直接保存在父帧，省去共享状态分配及每个分支的引用计数更新。
  // 启动失败、赢家异常和正常返回都先等待 all_done，不提前销毁这块存储。
  detail::select_state<Ts...> state_storage;
  auto* state = &state_storage;
  // 读取父任务停止令牌，稍后转发到本次选择的停止源。
  auto parent_stop = co_await this_coro::stop_token();
  // 回调保存在父 select 帧，覆盖启动、等待赢家和排空阶段。
  std::optional<std::stop_callback<detail::forward_select_stop>> parent_callback;
  // 不可取消的父任务不注册停止回调。
  if (parent_stop.stop_possible())
    // 父停止时请求所有候选停止，已停止父令牌也会立即同步转发。
    parent_callback.emplace(parent_stop, detail::forward_select_stop{&state->stop});
  // 区别启动失败与用户孩子异常；启动失败先排空再优先重抛。
  std::exception_ptr launch_error;
  try {
    // 按本次轮转顺序提交所有候选，把输入帧所有权移交出去。
    detail::start_select(state, std::index_sequence_for<Ts...>{}, std::move(children)...);
  // 先保存启动错误，不能立即离开并销毁仍被孩子借用的槽位。
  } catch (...) { launch_error = std::current_exception(); }
  // 全部提交尝试结束，归还启动哨兵，让计数可以归零。
  state->child_done();
  // 启动失败时没有必要等合法赢家，但必须等已有候选退出。
  if (launch_error) {
    // 不可提前取消的排空等待，保护孩子引用的对象和父帧生命周期。
    co_await state->all_done.wait();
    // 启动失败排空后重抛原始错误。
    std::rethrow_exception(launch_error);
  }
  // 先等待胜出者发布完整值或异常，不能只读取 claimed 就认为结果就绪。
  co_await state->completion.wait();
  // 不可提前取消的排空等待，保护孩子引用的对象和父帧生命周期。
  co_await state->all_done.wait();
  // 所有候选已退出，此时向调用者传播赢家异常或结果移动异常。
  if (state->error) std::rethrow_exception(state->error);
  // 返回原输入下标及对应 variant 值；落选结果在分支退出时丢弃。
  co_return select_result<Ts...>{state->index, std::move(*state->value)};
}
} // namespace faio
#endif
