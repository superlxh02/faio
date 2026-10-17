/**
 * @file faio/detail/experimental/execution/task_sender.hpp
 * @brief 实验性的入口注解与发送器接收器适配：task 与发送器桥接。
 * @details 实验适配在编译期开关下启用，连接阶段建立稳定操作状态；启动、结果交付与关闭沿受控任务图的租约和排空协议推进。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_TASK_SENDER_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_TASK_SENDER_HPP
#include "faio/detail/experimental/execution/environment.hpp"
#include "faio/detail/coroutine/root_task.hpp"
#include <atomic>
#include <optional>

namespace faio::experimental::detail {
template <class T> struct task_value {
  std::optional<T> value;  ///< 桥接协程保存的返回值，全部子任务结束后交付接收器。
};
template <> struct task_value<void> {};

/// @brief 协程帧仅依赖 faio 结果类型，不依赖后端接收器类型，避免将后端局部 lambda 类型带入帧类型。
template <class T> struct task_bridge_body : task_value<T> {
  explicit task_bridge_body(root_handle graph) : root(std::move(graph)) {}
  root_handle root;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
  std::stop_source stop;  ///< 本操作或任务图的停止源，向已登记等待者广播协作取消。
  faio::detail::task_tracker tracker;  ///< 跟踪本任务及派生任务的完成计数，归零后才允许排空返回。
  std::exception_ptr error;  ///< 跨异步边界保存的异常，结果领取时重新抛出。
  bool cancelled{};  ///< 本次操作是否以取消原因结束。
  std::atomic<bool> body_done{false};  ///< 桥接协程体是否结束；派生任务可能尚未排空。
};

template <class T, class Receiver>
struct task_bridge_state : task_bridge_body<T>, std::enable_shared_from_this<task_bridge_state<T, Receiver>> {
  using body_type = task_bridge_body<T>;  ///< 只依赖 faio 结果类型的桥接协程体状态类型。
  using body_type::root;
  using body_type::stop;
  using body_type::tracker;
  using body_type::error;
  using body_type::cancelled;
  using body_type::body_done;
  using receiver_token = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;  ///< 从接收器环境推导的停止令牌类型。
  struct request_stop {
    task_bridge_state* state;  ///< 关联操作的状态引用或指针，使用期间须由外部所有者保持存活。
    void operator()() const noexcept { state->stop.request_stop(); }
  };
  struct terminal_node_type : faio::detail::external_work_node { task_bridge_state* state;  ///< 关联操作的状态引用或指针，使用期间须由外部所有者保持存活。
  } terminal_node;  ///< 嵌入稳定状态的终态节点，在受控宿主上交付结果。
  faio::runtime::detail::runtime_context* context;  ///< 借用运行时上下文，其生命期必须覆盖本操作。
  bool independent;  ///< 本次操作是否独立持有根任务接纳责任。
  std::optional<Receiver> receiver;  ///< 结果交付接收器，完成时移出以允许回调销毁当前操作。
  std::optional<stop_callback<receiver_token, request_stop>> receiver_link;  ///< 停止源声明在回调之前，逆序析构使停止源存活到回调注销之后。
  std::optional<std::stop_callback<request_stop>> root_link;  ///< 将根任务停止请求连接到桥接停止源的回调。
  std::atomic<bool> tracker_zero{false},  ///< 桥接体及全部受跟踪子任务是否已经归零。
      terminal_queued{false};  ///< 终态节点是否已发布，原子仲裁确保只发布一次。
  std::atomic<bool> delivered{false};  ///< 接收器终态是否已交付，防止重复完成。
  std::atomic<std::size_t> publishers{0};  ///< 尚未退出的结果发布者数量，归零后才允许终态清理。
  std::shared_ptr<task_bridge_state> terminal_keepalive;  ///< 覆盖终态节点排队至交付期间的共享状态租约。

  task_bridge_state(faio::runtime::detail::runtime_context& target, root_handle graph,
                    bool owns, Receiver consumer)
      : body_type(std::move(graph)), context(&target), independent(owns), receiver(std::move(consumer)) {
    root->child_begin();
    terminal_node.execute = &deliver;
    terminal_node.scope = {&root->host(), root.get()};
    terminal_node.state = this;
    try {
      receiver_link.emplace(stdexec::get_stop_token(stdexec::get_env(*receiver)), request_stop{this});
      root_link.emplace(root->stop_token(), request_stop{this});
      tracker.install_zero_completion_hook(this, &zero, &begin_publisher, &end_publisher);
    } catch (...) {
      root->child_end();
      throw;
    }
  }
  ~task_bridge_state() {
    /// @brief 从未启动的桥接对象既没有包装任务凭证，也没有已入队的终态节点。
    if (!terminal_queued.load(std::memory_order_relaxed)) {
      if (independent) root->terminal();
      root->child_end();
    }
  }
  static void begin_publisher(void* address) noexcept {
    static_cast<task_bridge_state*>(address)->publishers.fetch_add(1, std::memory_order_acq_rel);
  }
  static void end_publisher(void* address) noexcept {
    /// @brief 递减计数前先取得独立租约；另一个最后发布者可在本调用返回前将终态节点入队并交付。
    auto state = static_cast<task_bridge_state*>(address)->shared_from_this();
    if (state->publishers.fetch_sub(1, std::memory_order_acq_rel) == 1)
      state->try_terminal();
  }
  static void zero(void* address) noexcept {
    auto* state = static_cast<task_bridge_state*>(address);
    state->tracker_zero.store(true, std::memory_order_release);
    state->try_terminal();
  }
  void try_terminal() noexcept {
    if (!body_done.load(std::memory_order_acquire) || !tracker_zero.load(std::memory_order_acquire)
        || publishers.load(std::memory_order_acquire) != 0)
      return;
    if (terminal_queued.exchange(true, std::memory_order_acq_rel)) return;
    terminal_keepalive = this->shared_from_this();
    root->host().publish(terminal_node);
  }
  static void deliver(faio::detail::external_work_node* node) noexcept {
    auto* raw = static_cast<terminal_node_type*>(node)->state;
    auto state = std::move(raw->terminal_keepalive);
    auto root = state->root;
    state->delivered.store(true, std::memory_order_release);
    auto receiver = std::move(*state->receiver);
    state->receiver.reset();
    state->receiver_link.reset();
    state->root_link.reset();
    if (state->independent) root->terminal();
    /// @brief 子任务凭证覆盖接收器交付及其可能同步连接的后代；局部租约在操作对象自销毁后继续有效。
    if (state->error) stdexec::set_error(std::move(receiver), state->error);
    else if (state->cancelled) stdexec::set_stopped(std::move(receiver));
    else if constexpr (std::is_void_v<T>) stdexec::set_value(std::move(receiver));
    else stdexec::set_value(std::move(receiver), std::move(*state->value));
    root->child_end();
  }
};

template <class T>
auto bridge_coroutine(std::shared_ptr<task_bridge_body<T>> state, faio::task<T> child)
    -> faio::detail::detached_task {
  faio::detail::current_tracker = &state->tracker;
  faio::detail::current_stop_token = state->stop.get_token();
  faio::detail::current_cancellation_policy = faio::detail::cancellation_error_policy::normal_if_stop_requested;
  faio::detail::current_external_scope = {&state->root->host(), state->root.get()};
  /// @brief 桥接计数器持有自身取消回调链，直到所有子任务
  /// 退出后才解除取消连接；不从执行连接操作的线程继承取消所有者。
  faio::detail::current_cancellation_owner.reset();
  try {
    if constexpr (std::is_void_v<T>) co_await std::move(child);
    else state->value.emplace(co_await std::move(child));
  } catch (const faio::operation_cancelled&) {
    if (state->stop.stop_requested()) state->cancelled = true;
    else state->error = std::current_exception();
  } catch (...) { state->error = std::current_exception(); }
  state->body_done.store(true, std::memory_order_release);
  /// @brief promise 析构仅在本协程体及全部子根任务结束后发布 tracker_zero；完成守卫覆盖 lifetime.finish_task()。
}

template <class T, class Receiver>
class task_operation {
  using state_type = task_bridge_state<T, Receiver>;  ///< 本桥接对象使用的稳定共享状态类型。
  struct start_node_type : faio::detail::external_work_node { task_operation* owner;  ///< 借用所属对象的稳定地址，回调或等待期间不得迁移或销毁。
  } start_node_;  ///< 嵌入操作状态的启动节点，将执行转交到目标宿主。
 public:
  using operation_state_concept = stdexec::operation_state_tag;  ///< 标记该类型为连接后地址稳定的操作状态。
  task_operation(faio::runtime::detail::runtime_context& context, root_handle bound,
                 faio::task<T> child, Receiver receiver)
      : state_(make_state(context, std::move(bound), std::move(receiver))),
        reservation_(context.external_host().reserve(state_->root, state_->independent)),
        wrapper_(bridge_coroutine<T>(state_, std::move(child))) {
    start_node_.owner = this;
    start_node_.execute = &dispatch;
    start_node_.scope = {&context.external_host(), state_->root.get()};
  }
  task_operation(const task_operation&) = delete;
  task_operation(task_operation&&) = delete;
  ~task_operation() {
    if (started_ && !state_->delivered.load(std::memory_order_acquire)) std::terminate();
  }
  /// @brief 把已连接的稳定操作交给其执行协议，之后只能交付一个终态。
  void start() noexcept {
    if (std::exchange(started_, true)) std::terminate();
    auto reservation = std::move(reservation_);
    state_->root->host().publish(start_node_);
    reservation.start();
  }
 private:
  static auto make_state(faio::runtime::detail::runtime_context& context, root_handle bound, Receiver receiver)
      -> std::shared_ptr<state_type> {
    auto& host = context.external_host();
    bool independent = !(bound && bound->valid());
    if constexpr (requires { get_external_root(stdexec::get_env(receiver)); }) {
      auto visible = get_external_root(stdexec::get_env(receiver));
      if (visible && visible->valid() && &visible->host() == &host) independent = false;
    }
    if (current_root(host)) independent = false;
    auto root = resolve_root(host, std::move(bound), stdexec::get_env(receiver));
    return std::make_shared<state_type>(context, std::move(root), independent, std::move(receiver));
  }
  static void dispatch(faio::detail::external_work_node* node) noexcept {
    auto& operation = *static_cast<start_node_type*>(node)->owner;
    auto state = operation.state_;
    auto wrapper = std::move(operation.wrapper_);
    state->tracker.add();
    wrapper.set_completion({}, &state->tracker);
    try {
      auto scheduler = state->context->scheduler_for_external_root(state->root);
      auto lifetime = state->context->task_lifetime_for_external_root(state->root);
      faio::detail::start_detached(std::move(wrapper), scheduler, &state->tracker, lifetime);
    } catch (...) {
      state->error = std::current_exception();
      /// @brief 登记或调度失败时 start_detached 会销毁尚未提交的帧，并归还对应任务计数。
      state->body_done.store(true, std::memory_order_release);
      state->try_terminal();
    }
  }
  std::shared_ptr<state_type> state_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
  faio::detail::external_reservation reservation_;  ///< 启动前保留的任务图接纳租约，发布成功后转交执行责任。
  faio::detail::detached_task wrapper_;  ///< 承载桥接逻辑的根协程，启动时转交给运行时。
  bool started_{};  ///< 标记操作已启动，防止重复启动或误退还未启动预留。
};

template <class T> class task_sender {
 public:
  using sender_concept = stdexec::sender_tag;  ///< 标记该类型符合发送器接收器协议中的发送器角色。
  task_sender(faio::runtime::detail::runtime_context& context, faio::task<T> child)
      : context_(&context), root_(current_root(context.external_host())), child_(std::move(child)) {}
  task_sender(task_sender&&) = default;
  task_sender(const task_sender&) = delete;
  template <class Self, class Environment = stdexec::env<>>
  static consteval auto get_completion_signatures() noexcept {
    if constexpr (std::is_void_v<T>)
      return stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>{};
    else
      return stdexec::completion_signatures<stdexec::set_value_t(T), stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>{};
  }
  template <stdexec::receiver Receiver> auto connect(Receiver receiver) && -> task_operation<T, Receiver> {
    return {*context_, root_, std::move(child_), std::move(receiver)};
  }
 private:
  faio::runtime::detail::runtime_context* context_;  ///< 借用运行时上下文，其生命期必须覆盖本操作。
  root_handle root_;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
  faio::task<T> child_;  ///< 尚未启动的子任务，提交或等待时移交协程帧所有权。
};
}

namespace faio::experimental {
template <class T> auto runtime_ref::as_sender(faio::task<T> child) const {
  return detail::task_sender<T>{context(), std::move(child)};
}
}
#endif
