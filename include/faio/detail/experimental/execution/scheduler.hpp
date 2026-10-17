/**
 * @file faio/detail/experimental/execution/scheduler.hpp
 * @brief 实验性的入口注解与发送器接收器适配：调度接口。
 * @details 实验适配在编译期开关下启用，连接阶段建立稳定操作状态；启动、结果交付与关闭沿受控任务图的租约和排空协议推进。
 */
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
  using token_type = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;  ///< 从接收器环境推导的停止令牌类型。
 public:
  using operation_state_concept = stdexec::operation_state_tag;  ///< 标记该类型为连接后地址稳定的操作状态。
  schedule_operation(external_host& host, root_handle bound, Receiver receiver)
      : receiver_(std::move(receiver)), token_(stdexec::get_stop_token(stdexec::get_env(receiver_))),
        root_(resolve_root(host, bound, stdexec::get_env(receiver_))),
        owns_root_(!(bound && bound->valid()) && !bound_root_visible(host, stdexec::get_env(receiver_))),
        reservation_(host.reserve(root_, owns_root_)) {
    /// @brief 即使接收器隐藏环境查询，解析器仍可使用显式绑定的调度器；该根任务已归属受控任务图。
    node_.execute = &dispatch;
    node_.owner = this;
    node_.scope = {&host, root_.get()};
  }
  schedule_operation(const schedule_operation&) = delete;
  schedule_operation(schedule_operation&&) = delete;
  schedule_operation& operator=(const schedule_operation&) = delete;
  schedule_operation& operator=(schedule_operation&&) = delete;
  ~schedule_operation() {
    /// @brief 调用方违反操作状态的生命期约束，不能在仍可能回调时释放操作。
    if (started_ && !terminal_)
      std::terminate();
  }
  /// @brief 把已连接的稳定操作交给其执行协议，之后只能交付一个终态。
  void start() noexcept {
    if (std::exchange(started_, true)) std::terminate();
    auto reservation = std::move(reservation_);
    root_->host().publish(node_);
    /// @brief 独立预留租约覆盖队列发布过程；完成回调可在 publish 返回前销毁操作对象，因此发布后仅访问局部变量。
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
  struct ready_node : faio::detail::external_work_node {
    schedule_operation* owner{};  ///< 借用所属对象的稳定地址，回调或等待期间不得迁移或销毁。
  } node_;  ///< 嵌入对象的稳定等待或分派节点，入队后直到完成交接前不得移动。
  static void dispatch(faio::detail::external_work_node* node) noexcept {
    auto& operation = *static_cast<ready_node*>(node)->owner;
    auto root = operation.root_;
    const bool stopped = operation.token_.stop_requested() || root->stop_token().stop_requested();
    const bool own = operation.owns_root_;
    operation.terminal_ = true;
    auto receiver = std::move(operation.receiver_);
    if (own) root->terminal();
    /// @brief 完成回调之后所用状态均独立于可能已被销毁的操作存储。
    if constexpr (!stdexec::unstoppable_token<token_type>) {
      if (stopped) { stdexec::set_stopped(std::move(receiver)); return; }
    }
    stdexec::set_value(std::move(receiver));
  }
  Receiver receiver_;  ///< 结果交付接收器，完成时移出以允许回调销毁当前操作。
  token_type token_;  ///< 从接收器环境捕获的停止令牌，用于启动与结果交付前的取消检查。
  root_handle root_;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
  bool owns_root_;  ///< 当前对象是否承担根任务完成责任，移动时成对转交。
  faio::detail::external_reservation reservation_;  ///< 启动前保留的任务图接纳租约，发布成功后转交执行责任。
  bool started_{};  ///< 标记操作已启动，防止重复启动或误退还未启动预留。
  bool terminal_{};  ///< 标记已交付终态，析构据此区分放弃与正常结束。
};
}

template <faio::runtime::mode Mode> class schedule_sender;

template <faio::runtime::mode Mode>
class execution_scheduler {
 public:
  using scheduler_concept = stdexec::scheduler_tag;  ///< 标记该类型符合发送器接收器协议中的调度器角色。
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
  /// @brief 取得关联运行时上下文的借用引用，不延长运行时生命期。
  auto context() const noexcept -> faio::runtime::detail::runtime_context& { return *context_; }
  auto root() const noexcept -> const detail::root_handle& { return root_; }
 private:
  faio::runtime::detail::runtime_context* context_;  ///< 借用运行时上下文，其生命期必须覆盖本操作。
  detail::root_handle root_;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
};

template <faio::runtime::mode Mode>
class schedule_sender {
 public:
  using sender_concept = stdexec::sender_tag;  ///< 标记该类型符合发送器接收器协议中的发送器角色。
  explicit schedule_sender(execution_scheduler<Mode> scheduler) noexcept : scheduler_(std::move(scheduler)) {}
  /// @brief 返回接收器环境视图，传播调度归属、停止令牌及原环境查询。
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
  execution_scheduler<Mode> scheduler_;  ///< 捕获的目标调度器，跨线程完成时将续体投递回所属运行时。
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
}
#endif
