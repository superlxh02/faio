#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SCHEDULER_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SCHEDULER_HPP
#include "faio/detail/experimental/runtime_facade.hpp"
#include "faio/detail/experimental/execution/backend.hpp"
#include "faio/detail/runtime/common/external_work_host.hpp"
#include <concepts>
#include <memory>
#include <type_traits>

namespace faio::experimental {
namespace detail {
using external_host = faio::detail::external_work_host;
using root_handle = external_host::root_handle;

struct get_external_root_t {
  static constexpr bool query(stdexec::forwarding_query_t) noexcept { return true; }
  template <class Environment>
    requires requires(const Environment& env, const get_external_root_t& query) { env.query(query); }
  auto operator()(const Environment& env) const noexcept { return env.query(*this); }
};
inline constexpr get_external_root_t get_external_root{};

inline auto current_root(external_host& host) noexcept -> root_handle {
  const auto scope = faio::detail::current_external_scope;
  if (scope.host == &host && scope.root_state && scope.root_state->valid())
    return scope.root_state->weak_from_this().lock();
  return {};
}

template <class Environment>
auto resolve_root(external_host& host, root_handle bound, const Environment& env) -> root_handle {
  if (bound && bound->valid()) {
    if (&bound->host() != &host)
      throw std::logic_error("faio experimental: cross-runtime root binding");
    return bound;
  }
  if constexpr (requires { get_external_root(env); }) {
    auto root = get_external_root(env);
    if (root && root->valid() && &root->host() == &host) {
      return root;
    }
  }
  if (auto root = current_root(host))
    return root;
  return host.acquire_root();
}

template <faio::runtime::mode Mode, class Receiver>
class schedule_operation {
  using token_type = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
 public:
  using operation_state_concept = stdexec::operation_state_tag;
  schedule_operation(external_host& host, root_handle bound, Receiver receiver)
      : receiver_(std::move(receiver)), token_(stdexec::get_stop_token(stdexec::get_env(receiver_))),
        root_(resolve_root(host, bound, stdexec::get_env(receiver_))),
        owns_root_(!(bound && bound->valid()) && !bound_root_visible(host, stdexec::get_env(receiver_))),
        reservation_(host.reserve(root_, owns_root_)) {
    // The resolver may use an explicitly bound scheduler even when the receiver
    // hides its queries. Such a root already belongs to a controlled graph.
    node_.execute = &dispatch;
    node_.owner = this;
    node_.scope = {&host, root_.get()};
  }
  schedule_operation(const schedule_operation&) = delete;
  schedule_operation(schedule_operation&&) = delete;
  schedule_operation& operator=(const schedule_operation&) = delete;
  schedule_operation& operator=(schedule_operation&&) = delete;
  ~schedule_operation() {
    if (started_ && !terminal_)
      std::terminate(); // caller violated operation-state lifetime
  }
  void start() noexcept {
    if (std::exchange(started_, true)) std::terminate();
    auto reservation = std::move(reservation_);
    root_->host().publish(node_);
    // The independent reservation spans queue publication. Completion may
    // destroy this operation before publish returns; only locals are used.
    reservation.start();
  }
 private:
  template <class Environment>
  static bool bound_root_visible(external_host& host, const Environment& env) noexcept {
    if constexpr (requires { get_external_root(env); }) {
      auto root = get_external_root(env);
      if (root && root->valid() && &root->host() == &host) return true;
    }
    return static_cast<bool>(current_root(host));
  }
  struct ready_node : faio::detail::external_work_node { schedule_operation* owner{}; } node_;
  static void dispatch(faio::detail::external_work_node* node) noexcept {
    auto& operation = *static_cast<ready_node*>(node)->owner;
    auto root = operation.root_;
    const bool stopped = operation.token_.stop_requested() || root->stop_token().stop_requested();
    const bool own = operation.owns_root_;
    operation.terminal_ = true;
    auto receiver = std::move(operation.receiver_);
    if (own) root->terminal();
    // Everything used after completion is independent of operation storage.
    if constexpr (!stdexec::unstoppable_token<token_type>) {
      if (stopped) { stdexec::set_stopped(std::move(receiver)); return; }
    }
    stdexec::set_value(std::move(receiver));
  }
  Receiver receiver_;
  token_type token_;
  root_handle root_;
  bool owns_root_;
  faio::detail::external_reservation reservation_;
  bool started_{};
  bool terminal_{};
};
} // namespace detail

template <faio::runtime::mode Mode> class schedule_sender;

template <faio::runtime::mode Mode>
class execution_scheduler {
 public:
  using scheduler_concept = stdexec::scheduler_tag;
  explicit execution_scheduler(faio::runtime::detail::runtime_context& context,
                               detail::root_handle root = {}) noexcept
      : context_(&context), root_(std::move(root)) {}
  auto schedule() const noexcept -> schedule_sender<Mode>;
  template <class Completion>
    requires std::same_as<Completion, stdexec::set_value_t> || std::same_as<Completion, stdexec::set_stopped_t>
  auto query(stdexec::get_completion_scheduler_t<Completion>) const noexcept -> execution_scheduler { return *this; }
  template <class Completion>
    requires std::same_as<Completion, stdexec::set_value_t> || std::same_as<Completion, stdexec::set_stopped_t>
  auto query(stdexec::get_completion_domain_t<Completion>) const noexcept -> stdexec::default_domain { return {}; }
  auto query(stdexec::get_domain_t) const noexcept -> stdexec::default_domain { return {}; }
  auto query(stdexec::get_forward_progress_guarantee_t) const noexcept -> stdexec::forward_progress_guarantee {
    return Mode == faio::runtime::mode::current_thread ? stdexec::forward_progress_guarantee::weakly_parallel
                                                    : stdexec::forward_progress_guarantee::parallel;
  }
  bool operator==(const execution_scheduler& other) const noexcept { return context_ == other.context_; }
  auto context() const noexcept -> faio::runtime::detail::runtime_context& { return *context_; }
  auto root() const noexcept -> const detail::root_handle& { return root_; }
 private:
  faio::runtime::detail::runtime_context* context_;
  detail::root_handle root_;
};

template <faio::runtime::mode Mode>
class schedule_sender {
 public:
  using sender_concept = stdexec::sender_tag;
  explicit schedule_sender(execution_scheduler<Mode> scheduler) noexcept : scheduler_(std::move(scheduler)) {}
  auto get_env() const noexcept -> execution_scheduler<Mode> { return scheduler_; }
  template <class Self, class Environment = stdexec::env<>>
  static consteval auto get_completion_signatures() noexcept {
    using token = stdexec::stop_token_of_t<Environment>;
    if constexpr (stdexec::unstoppable_token<token>)
      return stdexec::completion_signatures<stdexec::set_value_t()>{};
    else
      return stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>{};
  }
  template <stdexec::receiver Receiver>
  auto connect(Receiver receiver) const -> detail::schedule_operation<Mode, Receiver> {
    return {scheduler_.context().external_host(), scheduler_.root(), std::move(receiver)};
  }
 private:
  execution_scheduler<Mode> scheduler_;
};
template <faio::runtime::mode Mode>
auto execution_scheduler<Mode>::schedule() const noexcept -> schedule_sender<Mode> { return schedule_sender<Mode>{*this}; }

inline auto runtime_ref::get_current_thread_scheduler() const -> current_thread_scheduler {
  if (mode() != faio::runtime::mode::current_thread)
    throw std::logic_error("faio experimental: current-thread scheduler unavailable");
  auto root = detail::current_root(context().external_host());
  if (!context().running() && !root)
    throw std::logic_error("faio experimental: current-thread scheduler unavailable");
  return current_thread_scheduler{context(), std::move(root)};
}
inline auto runtime_ref::get_multi_thread_scheduler() const -> multi_thread_scheduler {
  if (mode() != faio::runtime::mode::multi_thread)
    throw std::logic_error("faio experimental: multi-thread scheduler unavailable");
  auto root = detail::current_root(context().external_host());
  if (!context().running() && !root)
    throw std::logic_error("faio experimental: multi-thread scheduler unavailable");
  return multi_thread_scheduler{context(), std::move(root)};
}
template <faio::runtime::mode Mode>
auto detail::runtime_owner<Mode>::get_scheduler() noexcept -> execution_scheduler<Mode> {
  auto root = detail::current_root(context_.external_host());
  if (!context_.running() && !root) {
    std::fputs("faio experimental: get_scheduler on stopped runtime\n", stderr);
    std::terminate();
  }
  return execution_scheduler<Mode>{context_, std::move(root)};
}
static_assert(stdexec::scheduler<current_thread_scheduler>);
static_assert(stdexec::scheduler<multi_thread_scheduler>);
} // namespace faio::experimental
#endif
