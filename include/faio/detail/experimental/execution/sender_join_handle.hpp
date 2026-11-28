/**
 * @file faio/detail/experimental/execution/sender_join_handle.hpp
 * @brief 发送器观察句柄。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SENDER_JOIN_HANDLE_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_SENDER_JOIN_HANDLE_HPP
#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/experimental/execution/run.hpp"
#include <coroutine>

namespace faio::experimental::detail {
/** @brief 通过虚接口持有地址固定的 sender 操作。 */
struct graph_operation_storage {
  /** @brief 通过虚接口释放具体的 sender 操作。 */
  virtual ~graph_operation_storage() = default;
  /** @brief 启动类型擦除接口持有的 sender 操作，操作地址保持不变。 */
  virtual void start() noexcept = 0;
  /** @brief 返回关联任务图的根状态租约。 */
  virtual auto root() const noexcept -> root_handle = 0;
};

/** @brief 持有已提交任务图的结果、观察者与清理责任。 */
template <class Tuple>
struct submitted_graph : execution_result<Tuple>,
                         std::enable_shared_from_this<submitted_graph<Tuple>> {
  /** @brief 任务图排空后交付的清理节点。 */
  struct cleanup_node_type : faio::detail::external_work_node {
    submitted_graph* state; /**< 借用任务图状态，清理节点交付前由任务图自持有租约保护。 */
  } cleanup_node;           /**< 嵌入任务图的清理分派节点，其地址在排空前保持稳定。 */

  std::unique_ptr<graph_operation_storage>
      operation;    /**< 地址固定的底层异步操作，由当前包装负责启动、等待和交付结果。 */
  root_handle root; /**< 持有任务图的根状态租约，维持接纳、取消及排空的归属。 */
  std::shared_ptr<submitted_graph>
      execution_keepalive; /**< 执行期间持有自身的租约，避免任务图在发布终态前销毁。 */
  faio::detail::task_tracker*
      parent_tracker{};        /**< 借用父任务计数器，在子任务结束前归还计数责任。 */
  std::stop_token parent_stop; /**< 保存父级停止令牌，用于向当前任务图传播取消。 */
  std::shared_ptr<faio::detail::cancellation_state>
      parent_cancellation_owner; /**< 共享持有父级取消回调链，保证其存活。 */
  faio::detail::cancellation_error_policy cancellation_policy{
      faio::detail::cancellation_error_policy::
          FATAL_IF_UNOBSERVED}; /**< 随任务上下文传递的策略，用于处理未观察的取消异常。 */
  std::optional<std::stop_callback<request_root_stop>>
      parent_link; /**< 将父级停止请求转发到当前根任务的回调连接。 */
  std::atomic<bool> claimed{false}, abandoned{false},
      finished{false}; /**< 记录异步操作是否已经结束；发出取消请求不代表操作结束。 */
  std::atomic<unsigned> handshake{
      0}; /**< 0 表示无等待者，1 表示登记中，2 表示已挂起，3 表示已完成。 */
  faio::detail::external_work_node* waiter{}; /**< 等待结果的外部分派节点，完成时仅发布一次。 */

  /** @brief 创建提交结果并绑定任务图排空后的清理节点。 */
  explicit submitted_graph(external_host& host) : execution_result<Tuple>(host) {
    cleanup_node.template bind<&cleanup>(cleanup_node);
    cleanup_node.state = this;
  }

  /** @brief 检查失败是否属于父级允许的协作取消。 */
  bool matching_cancellation() const noexcept {
    if (cancellation_policy != faio::detail::cancellation_error_policy::NORMAL_IF_STOP_REQUESTED
        || !this->error || !root->stop_token().stop_requested())
      return false;
    try {
      std::rethrow_exception(this->error);
    } catch (const faio::operation_cancelled&) {
      return true;
    } catch (...) {
      return false;
    }
  }

  /** @brief 结束结果观察并检查未领取的失败责任。 */
  void abandon() noexcept {
    abandoned.store(true, std::memory_order_seq_cst);
    if (finished.load(std::memory_order_seq_cst) && this->error && !matching_cancellation())
      std::terminate();
  }

  /** @brief 释放已排空任务图的操作状态并发布观察结果。 */
  static void cleanup(cleanup_node_type* node) noexcept {
    auto* raw = node->state;
    auto state = std::move(raw->execution_keepalive);
    state->operation.reset();
    state->parent_link.reset();
    state->parent_cancellation_owner.reset();
    state->finished.store(true, std::memory_order_seq_cst);
    const auto prior = state->handshake.exchange(3, std::memory_order_acq_rel);
    if (prior == 2)
      state->host->publish(*state->waiter);
    if (state->abandoned.load(std::memory_order_seq_cst) && state->error
        && !state->matching_cancellation())
      std::terminate();
    if (auto* tracker = std::exchange(state->parent_tracker, nullptr)) {
      auto completion = tracker->done();
    }
  }
};
}  // namespace faio::experimental::detail

namespace faio::experimental {
/** @brief 可移动的 sender 句柄，负责观察已提交操作的结果。 */
template <class Tuple>
class sender_join_handle {
  using state_type = detail::submitted_graph<Tuple>; /**< 本桥接对象使用的稳定共享状态类型。 */
 public:
  /** @brief 接管 sender 结果，独占其观察责任。 */
  explicit sender_join_handle(std::shared_ptr<state_type> state) noexcept
      : state_(std::move(state)) {}

  /** @brief 接管 sender 结果，独占其观察责任。 */
  sender_join_handle(sender_join_handle&& other) noexcept : state_(std::move(other.state_)) {}

  /** @brief 禁止复制 sender 结果的观察责任。 */
  sender_join_handle(const sender_join_handle&) = delete;
  /** @brief 禁止复制独占的执行状态。 */
  sender_join_handle& operator=(const sender_join_handle&) = delete;

  /** @brief 转交观察责任并处理原有未领取状态。 */
  sender_join_handle& operator=(sender_join_handle&& other) noexcept {
    abandon();
    state_ = std::move(other.state_);
    return *this;
  }

  /** @brief 归还观察责任并检查未领取的失败。 */
  ~sender_join_handle() { abandon(); }

  /** @brief 保存在协程帧中的等待状态，用于在所属运行时等待 sender 结果。 */
  struct awaiter {
    std::shared_ptr<state_type>
        state;       /**< 共享持有操作状态，保证其在异步执行和领取结果期间地址不变。 */
    bool consumed{}; /**< 标记结果是否已消费，避免重复领取。 */

    struct resume_node : faio::detail::external_work_node {
      std::coroutine_handle<> handle; /**< 等待恢复的协程句柄，所有权由所属等待协议规定。 */
    } node;                           /**< 内嵌的恢复节点，从入队到完成交接期间不得移动。 */

    /** @brief 接管待观察结果并绑定协程恢复节点。 */
    explicit awaiter(std::shared_ptr<state_type> target) : state(std::move(target)) {
      node.template bind<+[](resume_node* work) noexcept {
        auto handle = work->handle;
        handle.resume();
      }>(node);
    }

    /** @brief 禁止复制或移动已登记的等待状态。 */
    awaiter(const awaiter&) = delete;
    /** @brief 禁止复制或移动已登记的等待状态。 */
    awaiter(awaiter&&) = delete;

    /** @brief 归还未消费的观察状态，禁止销毁已挂起的等待者。 */
    ~awaiter() {
      if (!consumed) {
        if (state->handshake.load(std::memory_order_acquire) == 2)
          std::terminate();
        state->abandon();
      }
    }

    /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
    bool await_ready() const noexcept { return state->finished.load(std::memory_order_acquire); }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    bool await_suspend(std::coroutine_handle<> handle) {
      auto* host = faio::detail::current_external_host();
      if (host != state->host)
        throw std::logic_error(
            "faio experimental: sender_join_handle must be awaited on its runtime");
      node.handle = handle;
      node.scope = ::faio::detail::current_task_context().external_scope;
      unsigned expected = 0;
      if (!state->handshake.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        if (expected == 3)
          return false;
        throw std::logic_error("faio experimental: multiple sender_join_handle waiters");
      }
      state->waiter = &node;
      return state->handshake.exchange(2, std::memory_order_acq_rel) != 3;
    }

    /** @brief 完成交接后领取结果，并按接口约定传播失败或取消。 */
    auto await_resume() -> std::optional<Tuple> {
      consumed = true;
      return state->take();
    }
  };

  /** @brief 消费观察句柄，创建一次性协程等待器。 */
  auto operator co_await() && -> awaiter { return consume(); }

 private:
  /** @brief 领取独占观察资格并转交共享结果状态。 */
  auto consume() -> awaiter {
    if (!state_ || state_->claimed.exchange(true, std::memory_order_acq_rel))
      throw std::logic_error("faio experimental: sender_join_handle result already consumed");
    return awaiter{std::move(state_)};
  }

  /** @brief 结束结果观察并检查未领取的失败责任。 */
  void abandon() noexcept {
    if (state_) {
      state_->abandon();
      state_.reset();
    }
  }

  std::shared_ptr<state_type>
      state_; /**< 共享持有操作状态，保证其在异步执行和领取结果期间地址不变。 */
};
}  // namespace faio::experimental
#endif
