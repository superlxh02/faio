#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_TASK_SENDER_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_TASK_SENDER_HPP
#include "faio/detail/experimental/execution/environment.hpp"
#include "faio/detail/coroutine/root_task.hpp"
#include <atomic>
#include <optional>

namespace faio::experimental::detail {
template <class T> struct task_value { std::optional<T> value; };
template <> struct task_value<void> {};

// Coroutine frames depend only on faio's result type, never on a backend's
// receiver type (which may contain an implementation-local lambda type).
template <class T> struct task_bridge_body : task_value<T> {
  explicit task_bridge_body(root_handle graph) : root(std::move(graph)) {}
  root_handle root;
  std::stop_source stop;
  faio::detail::task_tracker tracker;
  std::exception_ptr error;
  bool cancelled{};
  std::atomic<bool> body_done{false};
};

template <class T, class Receiver>
struct task_bridge_state : task_bridge_body<T>, std::enable_shared_from_this<task_bridge_state<T, Receiver>> {
  using body_type = task_bridge_body<T>;
  using body_type::root;
  using body_type::stop;
  using body_type::tracker;
  using body_type::error;
  using body_type::cancelled;
  using body_type::body_done;
  using receiver_token = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
  struct request_stop {
    task_bridge_state* state;
    void operator()() const noexcept { state->stop.request_stop(); }
  };
  struct terminal_node_type : faio::detail::external_work_node { task_bridge_state* state; } terminal_node;
  faio::runtime::detail::runtime_context* context;
  bool independent;
  std::optional<Receiver> receiver;
  // Sources precede callbacks and remain alive until callback unregister.
  std::optional<stop_callback<receiver_token, request_stop>> receiver_link;
  std::optional<std::stop_callback<request_stop>> root_link;
  std::atomic<bool> tracker_zero{false}, terminal_queued{false};
  std::atomic<bool> delivered{false};
  std::atomic<std::size_t> publishers{0};
  std::shared_ptr<task_bridge_state> terminal_keepalive;

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
    // A never-started bridge has no wrapper ticket or queued terminal node.
    if (!terminal_queued.load(std::memory_order_relaxed)) {
      if (independent) root->terminal();
      root->child_end();
    }
  }
  static void begin_publisher(void* address) noexcept {
    static_cast<task_bridge_state*>(address)->publishers.fetch_add(1, std::memory_order_acq_rel);
  }
  static void end_publisher(void* address) noexcept {
    // Take an independent lease before dropping the counter. Another last
    // publisher may queue/deliver the terminal node before this call returns.
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
    // The child ticket covers receiver delivery, which may synchronously
    // connect descendants. This local lease survives operation self-destruction.
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
  // The bridge tracker retains its own cancellation wiring until every child
  // finishes. Do not inherit cancellation ownership from a connecting thread.
  faio::detail::current_cancellation_owner.reset();
  try {
    if constexpr (std::is_void_v<T>) co_await std::move(child);
    else state->value.emplace(co_await std::move(child));
  } catch (const faio::operation_cancelled&) {
    if (state->stop.stop_requested()) state->cancelled = true;
    else state->error = std::current_exception();
  } catch (...) { state->error = std::current_exception(); }
  state->body_done.store(true, std::memory_order_release);
  // The promise destructor publishes tracker_zero only after this body and all
  // child roots finish. Its completion guard spans lifetime.finish_task().
}

template <class T, class Receiver>
class task_operation {
  using state_type = task_bridge_state<T, Receiver>;
  struct start_node_type : faio::detail::external_work_node { task_operation* owner; } start_node_;
 public:
  using operation_state_concept = stdexec::operation_state_tag;
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
      // start_detached destroys the unsubmitted frame and returns its tracker
      // ticket even if registration/scheduling fails.
      state->body_done.store(true, std::memory_order_release);
      state->try_terminal();
    }
  }
  std::shared_ptr<state_type> state_;
  faio::detail::external_reservation reservation_;
  faio::detail::detached_task wrapper_;
  bool started_{};
};

template <class T> class task_sender {
 public:
  using sender_concept = stdexec::sender_tag;
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
  faio::runtime::detail::runtime_context* context_;
  root_handle root_;
  faio::task<T> child_;
};
} // namespace faio::experimental::detail

namespace faio::experimental {
template <class T> auto runtime_ref::as_sender(faio::task<T> child) const {
  return detail::task_sender<T>{context(), std::move(child)};
}
} // namespace faio::experimental
#endif
