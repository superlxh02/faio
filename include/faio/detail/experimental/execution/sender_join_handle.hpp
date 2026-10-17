/**
 * @file faio/detail/experimental/execution/sender_join_handle.hpp
 * @brief 实验性的入口注解与发送器接收器适配：发送器观察句柄。
 * @details 实验适配在编译期开关下启用，连接阶段建立稳定操作状态；启动、结果交付与关闭沿受控任务图的租约和排空协议推进。
 */
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
  struct cleanup_node_type : faio::detail::external_work_node {
    submitted_graph* state;  ///< 借用任务图状态，清理节点交付前由任务图自持有租约保护。
  } cleanup_node;  ///< 嵌入任务图的清理分派节点，其地址在排空前保持稳定。
  std::unique_ptr<graph_operation_storage> operation;  ///< 地址稳定的底层异步操作，当前包装负责其启动、等待与结果交付。
  root_handle root;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
  std::shared_ptr<submitted_graph> execution_keepalive;  ///< 执行期间的自持有租约，防止任务图在终态发布前销毁。
  faio::detail::task_tracker* parent_tracker{};  ///< 借用父任务计数器，子任务终止前归还其计数责任。
  std::stop_token parent_stop;  ///< 捕获的父停止令牌，用于向当前任务图传播取消。
  std::shared_ptr<faio::detail::cancellation_state> parent_cancellation_owner;  ///< 保持父级取消回调链存活的共享租约。
  faio::detail::cancellation_error_policy cancellation_policy{
      faio::detail::cancellation_error_policy::fatal_if_unobserved};  ///< 未观察的取消异常处理策略，随任务上下文传播。
  std::optional<std::stop_callback<request_root_stop>> parent_link;  ///< 将父级停止请求转发到当前根任务的回调连接。
  std::atomic<bool> claimed{false},  ///< 结果是否已由唯一消费者领取，防止重复等待。
      abandoned{false},  ///< 观察句柄是否已被放弃，终态处理据此回收无人领取的状态。
      finished{false};  ///< 异步操作是否已真实结束，区别于仅请求取消。
  std::atomic<unsigned> handshake{0};  ///< 0 表示无等待者，1 表示登记中，2 表示已挂起，3 表示已完成。
  faio::detail::external_work_node* waiter{};  ///< 等待结果的外部分派节点，完成时仅发布一次。
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
  /// @brief 取得专门执行不可丢弃关闭工作的清理服务引用。
  static void cleanup(faio::detail::external_work_node* node) noexcept {
    auto* raw = static_cast<cleanup_node_type*>(node)->state;
    auto state = std::move(raw->execution_keepalive);
    /// @brief on_drained 仅在任务图的分派、发布与桥接责任全部归还后触发，此时其他线程才可安全释放稳定操作存储。
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
      /// @brief 完成守卫独立保留计数器所有者，覆盖其最后的结果发布尾段。
    }
  }
};
}

namespace faio::experimental {
template <class Tuple>
class sender_join_handle {
  using state_type = detail::submitted_graph<Tuple>;  ///< 本桥接对象使用的稳定共享状态类型。
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
    std::shared_ptr<state_type> state;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
    bool consumed{};  ///< 结果或输入是否已消费，防止重复领取。
    struct resume_node : faio::detail::external_work_node { std::coroutine_handle<> handle;  ///< 当前持有或等待恢复的协程帧句柄，所有权遵循所属等待协议。
    } node;  ///< 嵌入对象的稳定等待或分派节点，入队后直到完成交接前不得移动。
    explicit awaiter(std::shared_ptr<state_type> target) : state(std::move(target)) {
      node.execute = +[](faio::detail::external_work_node* work) noexcept {
        auto handle = static_cast<resume_node*>(work)->handle;
        /// @brief 恢复可立即销毁当前等待器，调用后不再访问其成员。
        handle.resume();
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
    /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
    bool await_ready() const noexcept { return state->finished.load(std::memory_order_acquire); }
    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
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
    /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
    auto await_resume() -> std::optional<Tuple> { consumed = true; return state->take(); }
  };
  auto operator co_await() && -> awaiter { return consume(); }
 private:
  /// @brief 确认已消费缓冲区前缀，并推进读取位置。
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
  std::shared_ptr<state_type> state_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
};
}
#endif
