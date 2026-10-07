#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SENDER_JOIN_HANDLE_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SENDER_JOIN_HANDLE_HPP
#include "faio/detail/experimental/execution/run.hpp"
#include <coroutine>

namespace faio::experimental::detail {
struct graph_operation_storage {
  virtual ~graph_operation_storage() = default;
  virtual void start() noexcept = 0;
  virtual auto root() const noexcept -> root_handle = 0;
};
template <class Tuple>
struct submitted_graph : execution_result<Tuple>, std::enable_shared_from_this<submitted_graph<Tuple>> {
  struct cleanup_node_type : faio::detail::external_work_node { submitted_graph* state; } cleanup_node;
  std::unique_ptr<graph_operation_storage> operation;
  root_handle root;
  std::shared_ptr<submitted_graph> execution_keepalive;
  faio::detail::task_tracker* parent_tracker{};
  std::stop_token parent_stop;
  std::shared_ptr<faio::detail::cancellation_state> parent_cancellation_owner;
  faio::detail::cancellation_error_policy cancellation_policy{
      faio::detail::cancellation_error_policy::fatal_if_unobserved};
  std::optional<std::stop_callback<request_root_stop>> parent_link;
  std::atomic<bool> claimed{false}, abandoned{false}, finished{false};
  // 0: no waiter, 1: registering, 2: suspended, 3: completed.
  std::atomic<unsigned> handshake{0};
  faio::detail::external_work_node* waiter{};
  explicit submitted_graph(external_host& host) : execution_result<Tuple>(host) {
    cleanup_node.execute = &cleanup;
    cleanup_node.state = this;
  }
  bool matching_cancellation() const noexcept {
    if (cancellation_policy != faio::detail::cancellation_error_policy::normal_if_stop_requested
        || !this->error || !root->stop_token().stop_requested()) return false;
    try { std::rethrow_exception(this->error); }
    catch (const faio::operation_cancelled&) { return true; }
    catch (...) { return false; }
  }
  void abandon() noexcept {
    abandoned.store(true, std::memory_order_seq_cst);
    if (finished.load(std::memory_order_seq_cst) && this->error && !matching_cancellation())
      std::terminate();
  }
  static void cleanup(faio::detail::external_work_node* node) noexcept {
    auto* raw = static_cast<cleanup_node_type*>(node)->state;
    auto state = std::move(raw->execution_keepalive);
    // on_drained is fired after all graph dispatch/publisher/bridge tickets
    // retire. Releasing stable operation storage is now safe on another worker.
    state->operation.reset();
    state->parent_link.reset();
    state->parent_cancellation_owner.reset();
    state->finished.store(true, std::memory_order_seq_cst);
    const auto prior = state->handshake.exchange(3, std::memory_order_acq_rel);
    if (prior == 2) state->host->publish(*state->waiter);
    if (state->abandoned.load(std::memory_order_seq_cst) && state->error && !state->matching_cancellation())
      std::terminate();
    if (auto* tracker = std::exchange(state->parent_tracker, nullptr)) {
      auto completion = tracker->done();
      // completion independently pins the tracker owner's publisher tail.
    }
  }
};
} // namespace faio::experimental::detail

namespace faio::experimental {
template <class Tuple>
class sender_join_handle {
  using state_type = detail::submitted_graph<Tuple>;
 public:
  explicit sender_join_handle(std::shared_ptr<state_type> state) noexcept : state_(std::move(state)) {}
  sender_join_handle(sender_join_handle&& other) noexcept : state_(std::move(other.state_)) {}
  sender_join_handle(const sender_join_handle&) = delete;
  sender_join_handle& operator=(const sender_join_handle&) = delete;
  sender_join_handle& operator=(sender_join_handle&& other) noexcept {
    abandon(); state_ = std::move(other.state_); return *this;
  }
  ~sender_join_handle() { abandon(); }
  struct awaiter {
    std::shared_ptr<state_type> state;
    bool consumed{};
    struct resume_node : faio::detail::external_work_node { std::coroutine_handle<> handle; } node;
    explicit awaiter(std::shared_ptr<state_type> target) : state(std::move(target)) {
      node.execute = +[](faio::detail::external_work_node* work) noexcept {
        auto handle = static_cast<resume_node*>(work)->handle;
        handle.resume(); // resume can destroy this awaiter; no access follows
      };
    }
    awaiter(const awaiter&) = delete;
    awaiter(awaiter&&) = delete;
    ~awaiter() {
      if (!consumed) {
        if (state->handshake.load(std::memory_order_acquire) == 2) std::terminate();
        state->abandon();
      }
    }
    bool await_ready() const noexcept { return state->finished.load(std::memory_order_acquire); }
    bool await_suspend(std::coroutine_handle<> handle) {
      auto* host = faio::detail::current_external_host();
      if (host != state->host)
        throw std::logic_error("faio experimental: sender_join_handle must be awaited on its runtime");
      node.handle = handle;
      node.scope = faio::detail::current_external_scope;
      unsigned expected = 0;
      if (!state->handshake.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        if (expected == 3) return false;
        throw std::logic_error("faio experimental: multiple sender_join_handle waiters");
      }
      state->waiter = &node;
      return state->handshake.exchange(2, std::memory_order_acq_rel) != 3;
    }
    auto await_resume() -> std::optional<Tuple> { consumed = true; return state->take(); }
  };
  auto operator co_await() && -> awaiter { return consume(); }
 private:
  auto consume() -> awaiter {
    if (!state_ || state_->claimed.exchange(true, std::memory_order_acq_rel))
      throw std::logic_error("faio experimental: sender_join_handle result already consumed");
    return awaiter{std::move(state_)};
  }
  void abandon() noexcept {
    if (state_) {
      state_->abandon();
      state_.reset();
    }
  }
  std::shared_ptr<state_type> state_;
};
} // namespace faio::experimental
#endif
