/**
 * @file faio/detail/experimental/execution/task_sender.hpp
 * @brief task 与发送器桥接。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_TASK_SENDER_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_TASK_SENDER_HPP
#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/experimental/execution/environment.hpp"
#include "faio/detail/runtime/execution_adapter/task_completion_service.hpp"
#include "faio/detail/coroutine/launch/root_task.hpp"
#include <atomic>
#include <optional>

namespace faio::experimental::detail {
/** @brief 保存协程桥接成功值。 */
template <class T>
struct task_value {
  std::optional<T> value; /**< 桥接协程保存的返回值，全部子任务结束后交付接收器。 */
};

/** @brief 无返回值协程的桥接值存储。 */
template <>
struct task_value<void> {};

/** @brief 桥接共享状态基类仅依赖 faio 结果类型，避免将后端接收器类型带入协程帧。 */
template <class T>
struct task_bridge_state_base : task_value<T>, faio::detail::task_completion_service {
  /** @brief 为协程桥接结果接管所属任务图租约。 */
  explicit task_bridge_state_base(root_handle graph)
      : faio::detail::task_completion_service(std::move(graph)) {}

  std::stop_source stop; /**< 桥接任务的停止源，向已登记的等待者广播协作取消请求。 */
  faio::detail::task_tracker
      tracker;              /**< 跟踪本任务及其派生任务的完成数量，计数归零后才允许排空操作返回。 */
  std::exception_ptr error; /**< 跨异步边界保存异常，在领取结果时重新抛出。 */
  bool cancelled{};         /**< 本次操作是否以取消原因结束。 */
};

/** @brief 桥接状态，持有 receiver 和停止回调连接，并负责交付终态。 */
template <class T, class receiver_type>
struct task_bridge_state : task_bridge_state_base<T> {
  using base_type = task_bridge_state_base<T>; /**< 不依赖接收器类型的桥接共享状态基类。 */
  using base_type::root;
  using base_type::stop;
  using base_type::tracker;
  using base_type::error;
  using base_type::cancelled;
  using base_type::body_done;
  using base_type::tracker_zero;
  using base_type::terminal_queued;
  using base_type::publishers;
  using base_type::terminal_keepalive;
  using receiver_token = stdexec::stop_token_of_t<
      stdexec::env_of_t<receiver_type>>; /**< 从接收器环境推导的停止令牌类型。 */

  struct request_stop {
    task_bridge_state* state; /**< 借用桥接共享状态，外部所有者须保证它在使用期间存活。 */

    /** @brief 向桥接协程转发停止请求。 */
    void operator()() const noexcept { state->stop.request_stop(); }
  };

  /** @brief 在外部任务宿主交付协程终态的节点。 */
  struct terminal_node_type : faio::detail::external_work_node {
    task_bridge_state* state; /**< 借用桥接共享状态，外部所有者须保证它在使用期间存活。 */
  } terminal_node;            /**< 嵌入稳定状态的终态节点，在受控宿主上交付结果。 */

  faio::runtime::detail::runtime_context*
      context;      /**< 借用运行时上下文，运行时必须在整个操作期间保持存活。 */
  bool independent; /**< 本次操作是否独立持有根任务接纳责任。 */
  std::optional<receiver_type>
      receiver; /**< 接收操作结果的接收器，完成时将其移出，允许回调销毁当前操作。 */
  std::optional<stop_callback<receiver_token, request_stop>>
      receiver_link; /**< 停止源声明在回调之前，逆序析构使停止源存活到回调注销之后。 */
  std::optional<std::stop_callback<request_stop>>
      root_link; /**< 将根任务停止请求连接到桥接停止源的回调。 */
  std::atomic<bool> delivered{false};

  /** @brief 创建桥接状态，并连接父级和 receiver 的停止请求。 */
  task_bridge_state(faio::runtime::detail::runtime_context& target,
                    root_handle graph,
                    bool owns,
                    receiver_type consumer)
      : base_type(std::move(graph)),
        context(&target),
        independent(owns),
        receiver(std::move(consumer)) {
    root->child_begin();
    try {
      terminal_node.template bind<&deliver>(terminal_node);
      terminal_node.scope = {&root->host(), root.get()};
      terminal_node.state = this;
      this->bind_terminal_node(terminal_node);
      receiver_link.emplace(stdexec::get_stop_token(stdexec::get_env(*receiver)),
                            request_stop{this});
      root_link.emplace(root->stop_token(), request_stop{this});
      tracker.bind_completion(faio::detail::task_completion_ref{*this});
    } catch (...) {
      root->child_end();
      throw;
    }
  }

  /** @brief 归还尚未交付终态的根任务与子任务责任。 */
  ~task_bridge_state() {
    if (!terminal_queued.load(std::memory_order_relaxed)) {
      if (independent)
        root->terminal();
      root->child_end();
    }
  }

  /** @brief 在所属宿主交付协程终态并归还任务图子计数。 */
  static void deliver(terminal_node_type* node) noexcept {
    auto* raw = node->state;
    auto keepalive = std::move(raw->terminal_keepalive);
    auto* state = static_cast<task_bridge_state*>(keepalive.get());
    auto root = state->root;
    state->delivered.store(true, std::memory_order_release);
    auto receiver = std::move(*state->receiver);
    state->receiver.reset();
    state->receiver_link.reset();
    state->root_link.reset();
    if (state->independent)
      root->terminal();
    if (state->error)
      stdexec::set_error(std::move(receiver), state->error);
    else if (state->cancelled)
      stdexec::set_stopped(std::move(receiver));
    else if constexpr (std::is_void_v<T>)
      stdexec::set_value(std::move(receiver));
    else
      stdexec::set_value(std::move(receiver), std::move(*state->value));
    root->child_end();
  }
};

/** @brief 等待原协程并保存其值、异常或协作取消。 */
template <class T>
auto task_sender_root_task_wrapper(std::shared_ptr<task_bridge_state_base<T>> state,
                                   faio::task<T> child) -> faio::detail::root_task {
  try {
    if constexpr (std::is_void_v<T>)
      co_await std::move(child);
    else
      state->value.emplace(co_await std::move(child));
  } catch (const faio::operation_cancelled&) {
    if (state->stop.stop_requested())
      state->cancelled = true;
    else
      state->error = std::current_exception();
  } catch (...) {
    state->error = std::current_exception();
  }
  state->body_done.store(true, std::memory_order_release);
}

/** @brief 构造桥接根任务，并安装任务计数、停止令牌与外部任务图上下文。 */
template <class T>
auto make_task_sender_root_task(std::shared_ptr<task_bridge_state_base<T>> state,
                                faio::task<T> child) -> faio::detail::root_task {
  faio::detail::task_context context{
      .scope = &state->tracker,
      .stop_token = state->stop.get_token(),
      .cancellation_policy = faio::detail::cancellation_error_policy::NORMAL_IF_STOP_REQUESTED,
      .external_scope = {&state->root->host(), state->root.get()}};
  auto root = task_sender_root_task_wrapper(std::move(state), std::move(child));
  root.set_context(std::move(context));
  return root;
}

/** @brief 将惰性协程提交到运行时的 sender 操作。 */
template <class T, class receiver_type>
class task_operation {
  using state_type = task_bridge_state<T, receiver_type>; /**< 本桥接对象使用的稳定共享状态类型。 */

  struct start_node_type : faio::detail::external_work_node {
    task_operation* owner; /**< 借用所属操作；该操作在回调或等待期间不得移动或销毁。 */
  } start_node_;           /**< 嵌入操作状态的启动节点，将执行转交到目标宿主。 */
 public:
  using operation_state_concept =
      stdexec::operation_state_tag; /**< 标记该类型为操作状态，连接后其地址必须保持不变。 */

  /** @brief 预留任务图接纳责任并创建待投递的桥接协程。 */
  task_operation(faio::runtime::detail::runtime_context& context,
                 root_handle bound,
                 faio::task<T> child,
                 receiver_type receiver)
      : state_(make_state(context, std::move(bound), std::move(receiver))),
        reservation_(context.external_host().reserve(state_->root, state_->independent)),
        wrapper_(make_task_sender_root_task<T>(state_, std::move(child))) {
    start_node_.owner = this;
    start_node_.template bind<&dispatch>(start_node_);
    start_node_.scope = {&context.external_host(), state_->root.get()};
  }

  /** @brief 禁止复制或移动稳定操作状态。 */
  task_operation(const task_operation&) = delete;
  /** @brief 禁止复制或移动稳定操作状态。 */
  task_operation(task_operation&&) = delete;

  /** @brief 检查已启动的协程桥接是否交付终态。 */
  ~task_operation() {
    if (started_ && !state_->delivered.load(std::memory_order_acquire))
      std::terminate();
  }

  /** @brief 按执行协议启动已连接的操作，此后只能交付一次终态。 */
  void start() noexcept {
    if (std::exchange(started_, true))
      std::terminate();
    auto reservation = std::move(reservation_);
    state_->root->host().publish(start_node_);
    reservation.start();
  }

 private:
  /** @brief 选择所属任务图并创建协程桥接共享状态。 */
  static auto make_state(faio::runtime::detail::runtime_context& context,
                         root_handle bound,
                         receiver_type receiver) -> std::shared_ptr<state_type> {
    auto& host = context.external_host();
    bool independent = !(bound && bound->valid());
    if constexpr (requires { get_external_root(stdexec::get_env(receiver)); }) {
      auto visible = get_external_root(stdexec::get_env(receiver));
      if (visible && visible->valid() && &visible->host() == &host)
        independent = false;
    }
    if (current_root(host))
      independent = false;
    auto root = resolve_root(host, std::move(bound), stdexec::get_env(receiver));
    return std::make_shared<state_type>(context, std::move(root), independent, std::move(receiver));
  }

  /** @brief 在目标宿主启动桥接协程并登记子任务完成责任。 */
  static void dispatch(start_node_type* node) noexcept {
    auto& operation = *node->owner;
    auto state = operation.state_;
    auto wrapper = std::move(operation.wrapper_);
    state->tracker.add();
    wrapper.set_completion({}, &state->tracker);
    try {
      auto scheduler = state->context->scheduler_for_external_root(state->root);
      auto lifetime = state->context->task_lifetime_for_external_root(state->root);
      faio::detail::schedule_task(std::move(wrapper), scheduler, &state->tracker, lifetime);
    } catch (...) {
      state->error = std::current_exception();
      state->body_done.store(true, std::memory_order_release);
      state->try_terminal();
    }
  }

  std::shared_ptr<state_type>
      state_; /**< 共享持有操作状态，保证其在异步执行和领取结果期间地址不变。 */
  faio::detail::external_reservation
      reservation_;                 /**< 启动前预留的任务图接纳租约，发布成功后移交执行责任。 */
  faio::detail::root_task wrapper_; /**< 承载桥接逻辑的根任务，启动时转交给运行时。 */
  bool started_{}; /**< 记录操作是否已启动，防止重复启动，或误将预留按未启动状态退还。 */
};

/** @brief 持有待桥接协程及目标运行时的 sender。 */
template <class T>
class task_sender {
 public:
  using sender_concept = stdexec::sender_tag; /**< 标记该类型为 sender/receiver 协议中的发送器。 */

  /** @brief 捕获目标运行时、任务图与惰性协程。 */
  task_sender(faio::runtime::detail::runtime_context& context, faio::task<T> child)
      : context_(&context),
        root_(current_root(context.external_host())),
        child_(std::move(child)) {}

  /** @brief 移动待桥接协程的所有权。 */
  task_sender(task_sender&&) = default;
  /** @brief 禁止复制惰性协程所有权。 */
  task_sender(const task_sender&) = delete;

  /** @brief 声明 sender 在给定环境中的完成签名。 */
  template <class Self, class Environment = stdexec::env<>>
  static consteval auto get_completion_signatures() noexcept {
    if constexpr (std::is_void_v<T>)
      return stdexec::completion_signatures<stdexec::set_value_t(),
                                            stdexec::set_error_t(std::exception_ptr),
                                            stdexec::set_stopped_t()>{};
    else
      return stdexec::completion_signatures<stdexec::set_value_t(T),
                                            stdexec::set_error_t(std::exception_ptr),
                                            stdexec::set_stopped_t()>{};
  }

  /** @brief 连接 receiver，创建地址固定的异步操作。 */
  template <stdexec::receiver receiver_type>
  auto connect(receiver_type receiver) && -> task_operation<T, receiver_type> {
    return {*context_, root_, std::move(child_), std::move(receiver)};
  }

 private:
  faio::runtime::detail::runtime_context*
      context_;         /**< 借用运行时上下文，运行时必须在整个操作期间保持存活。 */
  root_handle root_;    /**< 持有任务图的根状态租约，维持接纳、取消及排空的归属。 */
  faio::task<T> child_; /**< 尚未启动的子任务，提交或等待时移交协程帧所有权。 */
};
}  // namespace faio::experimental::detail

namespace faio::experimental {
/** @brief 将惰性协程的所有权交给 sender 桥接对象。 */
template <class T>
auto runtime_ref::as_sender(faio::task<T> child) const {
  return detail::task_sender<T>{context(), std::move(child)};
}
}  // namespace faio::experimental
#endif
