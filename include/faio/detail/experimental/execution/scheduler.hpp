/**
 * @file faio/detail/experimental/execution/scheduler.hpp
 * @brief 调度接口。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SCHEDULER_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SCHEDULER_HPP
#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/experimental/runtime_facade.hpp"
#include "faio/detail/experimental/execution/backend.hpp"
#include "faio/detail/runtime/execution_adapter/external_work.hpp"
#include <concepts>
#include <memory>
#include <type_traits>

namespace faio::experimental {
namespace detail {
using external_host = faio::detail::external_work_host;
using root_handle = external_host::root_handle;

/** @brief 从 receiver 环境查询其所属的外部任务图。 */
struct get_external_root_t {
  /** @brief 标记任务图查询可随环境转发。 */
  static constexpr bool query(stdexec::forwarding_query_t) noexcept { return true; }

  /** @brief 读取环境提供的外部任务图租约。 */
  template <class Environment>
    requires requires(const Environment& env, const get_external_root_t& query) {
      env.query(query);
    }
  auto operator()(const Environment& env) const noexcept {
    return env.query(*this);
  }
};

inline constexpr get_external_root_t get_external_root{};

/** @brief 借用当前执行上下文中属于指定宿主的任务图。 */
inline auto current_root(external_host& host) noexcept -> root_handle {
  const auto scope = ::faio::detail::current_task_context().external_scope;
  if (scope.host == &host && scope.root_state && scope.root_state->valid())
    return scope.root_state->weak_from_this().lock();
  return {};
}

/** @brief 验证绑定的任务图或向宿主申请独立根任务。 */
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

/** @brief 将 receiver 的完成通知投递到目标运行时。 */
template <faio::runtime::mode Mode, class receiver_type>
class schedule_operation {
  using token_type = stdexec::stop_token_of_t<
      stdexec::env_of_t<receiver_type>>; /**< 从接收器环境推导的停止令牌类型。 */
 public:
  using operation_state_concept =
      stdexec::operation_state_tag; /**< 标记该类型为操作状态，连接后其地址必须保持不变。 */

  /** @brief 解析任务图并预留调度操作的接纳责任。 */
  schedule_operation(external_host& host, root_handle bound, receiver_type receiver)
      : receiver_(std::move(receiver)),
        token_(stdexec::get_stop_token(stdexec::get_env(receiver_))),
        root_(resolve_root(host, bound, stdexec::get_env(receiver_))),
        owns_root_(!(bound && bound->valid())
                   && !bound_root_visible(host, stdexec::get_env(receiver_))),
        reservation_(host.reserve(root_, owns_root_)) {
    node_.template bind<&dispatch>(node_);
    node_.owner = this;
    node_.scope = {&host, root_.get()};
  }

  /** @brief 禁止复制或移动调度操作的稳定地址。 */
  schedule_operation(const schedule_operation&) = delete;
  /** @brief 禁止复制或移动调度操作的稳定地址。 */
  schedule_operation(schedule_operation&&) = delete;
  /** @brief 禁止复制独占的执行状态。 */
  schedule_operation& operator=(const schedule_operation&) = delete;
  /** @brief 禁止移动稳定操作状态。 */
  schedule_operation& operator=(schedule_operation&&) = delete;

  /** @brief 检查已启动调度操作的终态责任。 */
  ~schedule_operation() {
    if (started_ && !terminal_)
      std::terminate();
  }

  /** @brief 按执行协议启动已连接的操作，此后只能交付一次终态。 */
  void start() noexcept {
    if (std::exchange(started_, true))
      std::terminate();
    auto reservation = std::move(reservation_);
    root_->host().publish(node_);
    reservation.start();
  }

 private:
  /** @brief 检查环境或当前执行上下文是否已绑定任务图。 */
  template <class Environment>
  static bool bound_root_visible(external_host& host, const Environment& env) noexcept {
    if constexpr (requires { get_external_root(env); }) {
      auto root = get_external_root(env);
      if (root && root->valid() && &root->host() == &host)
        return true;
    }
    return static_cast<bool>(current_root(host));
  }

  /** @brief 投递调度操作完成的稳定节点。 */
  struct ready_node : faio::detail::external_work_node {
    schedule_operation* owner{}; /**< 借用所属调度操作；该操作在回调或等待期间不得移动或销毁。 */
  } node_;                       /**< 内嵌的调度完成节点，从入队到完成交接期间不得移动。 */

  /** @brief 在目标运行时交付调度完成或停止终态。 */
  static void dispatch(ready_node* node) noexcept {
    auto& operation = *node->owner;
    auto root = operation.root_;
    const bool stopped = operation.token_.stop_requested() || root->stop_token().stop_requested();
    const bool own = operation.owns_root_;
    operation.terminal_ = true;
    auto receiver = std::move(operation.receiver_);
    if (own)
      root->terminal();
    if constexpr (!stdexec::unstoppable_token<token_type>) {
      if (stopped) {
        stdexec::set_stopped(std::move(receiver));
        return;
      }
    }
    stdexec::set_value(std::move(receiver));
  }

  receiver_type receiver_; /**< 接收操作结果的接收器，完成时将其移出，允许回调销毁当前操作。 */
  token_type token_;       /**< 从接收器环境捕获的停止令牌，用于启动与结果交付前的取消检查。 */
  root_handle root_;       /**< 持有任务图的根状态租约，维持接纳、取消及排空的归属。 */
  bool owns_root_;         /**< 标记本操作是否承担根任务的完成责任。 */
  faio::detail::external_reservation
      reservation_; /**< 启动前预留的任务图接纳租约，发布成功后移交执行责任。 */
  bool started_{};  /**< 记录操作是否已启动，防止重复启动，或误将预留按未启动状态退还。 */
  bool terminal_{}; /**< 记录是否已交付终态，析构时据此区分放弃操作与正常结束。 */
};
}  // namespace detail

/** @brief 在目标运行时交付完成通知的 sender。 */
template <faio::runtime::mode Mode>
class schedule_sender;

/** @brief 关联运行时与任务图的 sender 调度器。 */
template <faio::runtime::mode Mode>
class execution_scheduler {
 public:
  using scheduler_concept =
      stdexec::scheduler_tag; /**< 标记该类型为 sender/receiver 协议中的调度器。 */

  /** @brief 借用运行时并捕获可选的任务图租约。 */
  explicit execution_scheduler(faio::runtime::detail::runtime_context& context,
                               detail::root_handle root = {}) noexcept
      : context_(&context), root_(std::move(root)) {}

  /** @brief 创建在目标运行时完成的调度 sender。 */
  auto schedule() const noexcept -> schedule_sender<Mode>;

  /** @brief 返回完成值与停止终态使用的调度器。 */
  template <class Completion>
    requires std::same_as<Completion, stdexec::set_value_t>
             || std::same_as<Completion, stdexec::set_stopped_t>
  auto query(stdexec::get_completion_scheduler_t<Completion>) const noexcept
      -> execution_scheduler {
    return *this;
  }

  /** @brief 返回完成域的默认定制入口。 */
  template <class Completion>
    requires std::same_as<Completion, stdexec::set_value_t>
             || std::same_as<Completion, stdexec::set_stopped_t>
  auto query(stdexec::get_completion_domain_t<Completion>) const noexcept
      -> stdexec::default_domain {
    return {};
  }

  /** @brief 返回 sender 的默认执行域。 */
  auto query(stdexec::get_domain_t) const noexcept -> stdexec::default_domain { return {}; }

  /** @brief 返回当前调度模式的前向进展保证。 */
  auto query(stdexec::get_forward_progress_guarantee_t) const noexcept
      -> stdexec::forward_progress_guarantee {
    return Mode == faio::runtime::mode::CURRENT_THREAD
               ? stdexec::forward_progress_guarantee::weakly_parallel
               : stdexec::forward_progress_guarantee::parallel;
  }

  /** @brief 比较调度器是否引用同一运行时。 */
  bool operator==(const execution_scheduler& other) const noexcept {
    return context_ == other.context_;
  }

  /** @brief 取得关联运行时上下文的借用引用，不延长运行时生命期。 */
  auto context() const noexcept -> faio::runtime::detail::runtime_context& { return *context_; }

  /** @brief 返回关联任务图的根状态租约。 */
  auto root() const noexcept -> const detail::root_handle& { return root_; }

 private:
  faio::runtime::detail::runtime_context*
      context_;              /**< 借用运行时上下文，运行时必须在整个操作期间保持存活。 */
  detail::root_handle root_; /**< 持有任务图的根状态租约，维持接纳、取消及排空的归属。 */
};

/** @brief 在目标运行时交付完成通知的 sender。 */
template <faio::runtime::mode Mode>
class schedule_sender {
 public:
  using sender_concept = stdexec::sender_tag; /**< 标记该类型为 sender/receiver 协议中的发送器。 */

  /** @brief 捕获调度操作所需的运行时调度器。 */
  explicit schedule_sender(execution_scheduler<Mode> scheduler) noexcept
      : scheduler_(std::move(scheduler)) {}

  /** @brief 以运行时调度器提供 sender 的环境查询。 */
  auto get_env() const noexcept -> execution_scheduler<Mode> { return scheduler_; }

  /** @brief 声明 sender 在给定环境中的完成签名。 */
  template <class Self, class Environment = stdexec::env<>>
  static consteval auto get_completion_signatures() noexcept {
    using token = stdexec::stop_token_of_t<Environment>;
    if constexpr (stdexec::unstoppable_token<token>)
      return stdexec::completion_signatures<stdexec::set_value_t()>{};
    else
      return stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>{};
  }

  /** @brief 连接 receiver，创建地址固定的异步操作。 */
  template <stdexec::receiver receiver_type>
  auto connect(receiver_type receiver) const -> detail::schedule_operation<Mode, receiver_type> {
    return {scheduler_.context().external_host(), scheduler_.root(), std::move(receiver)};
  }

 private:
  execution_scheduler<Mode>
      scheduler_; /**< 保存目标调度器，跨线程完成时用它将续体投递回所属运行时。 */
};

/** @brief 创建在目标运行时完成的调度 sender。 */
template <faio::runtime::mode Mode>
auto execution_scheduler<Mode>::schedule() const noexcept -> schedule_sender<Mode> {
  return schedule_sender<Mode>{*this};
}

/** @brief 取得当前线程模式的调度器，模式不符时抛出异常。 */
inline auto runtime_ref::get_current_thread_scheduler() const -> current_thread_scheduler {
  if (mode() != faio::runtime::mode::CURRENT_THREAD)
    throw std::logic_error("faio experimental: current-thread scheduler unavailable");
  auto root = detail::current_root(context().external_host());
  if (!context().running() && !root)
    throw std::logic_error("faio experimental: current-thread scheduler unavailable");
  return current_thread_scheduler{context(), std::move(root)};
}

/** @brief 取得多工作线程模式的调度器；模式不符时抛出异常。 */
inline auto runtime_ref::get_multi_thread_scheduler() const -> multi_thread_scheduler {
  if (mode() != faio::runtime::mode::MULTI_THREAD)
    throw std::logic_error("faio experimental: multi-thread scheduler unavailable");
  auto root = detail::current_root(context().external_host());
  if (!context().running() && !root)
    throw std::logic_error("faio experimental: multi-thread scheduler unavailable");
  return multi_thread_scheduler{context(), std::move(root)};
}

/** @brief 返回与运行时固定模式匹配的调度器。 */
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
}  // namespace faio::experimental
#endif
