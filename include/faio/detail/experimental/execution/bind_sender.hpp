/**
 * @file faio/detail/experimental/execution/bind_sender.hpp
 * @brief 发送器运行时绑定。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_BIND_SENDER_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_BIND_SENDER_HPP
#include "faio/detail/experimental/execution/environment.hpp"
#include <optional>

namespace faio::experimental::detail {
/** @brief 绑定调度模式、任务图和接收器的稳定操作。 */
template <faio::runtime::mode Mode, class Sender, class Receiver>
class bound_operation {
  /** @brief 补充运行时环境，并转发 sender 的终态。 */
  struct forwarding_receiver {
    using receiver_concept =
        stdexec::receiver_tag; /**< 标记该类型为 sender/receiver 协议中的接收器。 */
    bound_operation* owner;    /**< 借用所属操作；该操作在回调或等待期间不得移动或销毁。 */

    /** @brief 返回接收器的环境视图，传递调度归属、停止令牌及原环境查询。 */
    auto get_env() const noexcept -> root_environment<Mode, stdexec::env_of_t<Receiver>> {
      return root_environment<Mode, stdexec::env_of_t<Receiver>>{
          owner->scheduler_, owner->root_, stdexec::get_env(owner->receiver_)};
    }

    /** @brief 向接收器交付成功值，交付后不再访问可被回调销毁的操作。 */
    template <class... Values>
    void set_value(Values&&... values) && noexcept {
      auto root = owner->root_;
      faio::detail::external_publisher_guard publisher{root};
      owner->terminal_ = true;
      auto receiver = std::move(owner->receiver_);
      root->terminal();
      stdexec::set_value(std::move(receiver), std::forward<Values>(values)...);
    }

    /** @brief 向接收器交付失败，交付后不再访问可被回调销毁的操作。 */
    template <class Error>
    void set_error(Error&& error) && noexcept {
      auto root = owner->root_;
      faio::detail::external_publisher_guard publisher{root};
      owner->terminal_ = true;
      auto receiver = std::move(owner->receiver_);
      root->terminal();
      stdexec::set_error(std::move(receiver), std::forward<Error>(error));
    }

    /** @brief 向接收器交付协作停止终态，归还任务图完成责任。 */
    void set_stopped() && noexcept {
      auto root = owner->root_;
      faio::detail::external_publisher_guard publisher{root};
      owner->terminal_ = true;
      auto receiver = std::move(owner->receiver_);
      root->terminal();
      stdexec::set_stopped(std::move(receiver));
    }
  };

  using token_type = stdexec::stop_token_of_t<
      stdexec::env_of_t<Receiver>>; /**< 从接收器环境推导的停止令牌类型。 */
  using started_sender = decltype(stdexec::starts_on(
      std::declval<execution_scheduler<Mode>>(),
      std::declval<Sender>())); /**< 绑定目标调度器后实际连接到接收器的发送器类型。 */
  using operation_type = stdexec::connect_result_t<
      started_sender,
      forwarding_receiver>; /**< 连接发送器与接收器后得到的稳定操作状态类型。 */
 public:
  using operation_state_concept =
      stdexec::operation_state_tag; /**< 标记该类型为操作状态，连接后其地址必须保持不变。 */

  /** @brief 连接源 sender，并预留目标任务图的接纳责任。 */
  bound_operation(faio::runtime::detail::runtime_context& context, Sender sender, Receiver receiver)
      : receiver_(std::move(receiver)),
        root_(context.external_host().acquire_root()),
        scheduler_(context, root_),
        reservation_(context.external_host().reserve(root_, true)),
        stop_(stdexec::get_stop_token(stdexec::get_env(receiver_)), request_root_stop{root_.get()}),
        operation_(stdexec::connect(stdexec::starts_on(scheduler_, std::move(sender)),
                                    forwarding_receiver{this})) {}

  /** @brief 禁止复制或移动稳定操作状态。 */
  bound_operation(const bound_operation&) = delete;
  /** @brief 禁止复制或移动稳定操作状态。 */
  bound_operation(bound_operation&&) = delete;

  /** @brief 检查已启动操作的终态交付责任。 */
  ~bound_operation() {
    if (started_ && !terminal_)
      std::terminate();
    if (!started_) {
      reservation_.reset();
      root_->terminal();
    }
  }

  /** @brief 按执行协议启动已连接的操作，此后只能交付一次终态。 */
  void start() noexcept {
    if (std::exchange(started_, true))
      std::terminate();
    reservation_.start();
    stdexec::start(operation_);
  }

  /** @brief 返回关联任务图的根状态租约。 */
  auto root() const noexcept -> const root_handle& { return root_; }

 private:
  Receiver receiver_; /**< 接收操作结果的接收器，完成时将其移出，允许回调销毁当前操作。 */
  root_handle root_;  /**< 持有任务图的根状态租约，维持接纳、取消及排空的归属。 */
  execution_scheduler<Mode>
      scheduler_; /**< 保存目标调度器，跨线程完成时用它将续体投递回所属运行时。 */
  faio::detail::external_reservation
      reservation_; /**< 启动前预留的任务图接纳租约，发布成功后移交执行责任。 */
  stop_callback<token_type, request_root_stop>
      stop_;                 /**< 将外部停止请求转发到本根任务的回调连接。 */
  bool started_{};           /**< 记录操作是否已启动，防止重复启动，或误将预留按未启动状态退还。 */
  bool terminal_{};          /**< 记录是否已交付终态，析构时据此区分放弃操作与正常结束。 */
  operation_type operation_; /**< 地址固定的底层异步操作，由当前包装负责启动、等待和交付结果。 */
};

/** @brief 保存固定调度模式及运行时的 sender。 */
template <faio::runtime::mode Mode, class Sender>
class bound_sender {
 public:
  using sender_concept = stdexec::sender_tag; /**< 标记该类型为 sender/receiver 协议中的发送器。 */

  /** @brief 保存目标模式、运行时和源 sender。 */
  bound_sender(faio::runtime::detail::runtime_context& context, Sender sender)
      : context_(&context), sender_(std::move(sender)) {}

  /** @brief 通过移动移交已绑定 sender 的所有权。 */
  bound_sender(bound_sender&&) = default;
  /** @brief 禁止复制源 sender 的所有权。 */
  bound_sender(const bound_sender&) = delete;

  /** @brief 声明 sender 在给定环境中的完成签名。 */
  template <class Self, class Environment = stdexec::env<>>
  static consteval auto get_completion_signatures() noexcept {
    return stdexec::completion_signatures_of_t<Sender, root_environment<Mode, Environment>>{};
  }

  /** @brief 连接 receiver，创建地址固定的异步操作。 */
  template <stdexec::receiver Receiver>
  auto connect(Receiver receiver) && -> bound_operation<Mode, Sender, Receiver> {
    return {*context_, std::move(sender_), std::move(receiver)};
  }

  /** @brief 转发源 sender 的环境查询。 */
  auto get_env() const noexcept { return stdexec::get_env(sender_); }

 private:
  faio::runtime::detail::runtime_context*
      context_;   /**< 借用运行时上下文，运行时必须在整个操作期间保持存活。 */
  Sender sender_; /**< 尚未连接或启动的源发送器对象。 */
};

/** @brief runtime_ref 在运行期选择模式；类型擦除仅发生在连接阶段，启动与完成期间不分配或迁移稳定内部操作。 */
template <class Sender, class Receiver>
class runtime_bound_operation {
  /** @brief 运行期模式选择后的类型擦除操作接口。 */
  struct base {
    /** @brief 通过虚接口释放具体的 sender 操作。 */
    virtual ~base() = default;
    /** @brief 启动类型擦除接口持有的 sender 操作，操作地址保持不变。 */
    virtual void start() noexcept = 0;
    /** @brief 返回关联任务图的根状态租约。 */
    virtual auto root() const noexcept -> root_handle = 0;
  };

  /** @brief 持有具体调度模式的稳定操作实现。 */
  template <faio::runtime::mode Mode>
  struct implementation final : base {
    bound_operation<Mode, Sender, Receiver>
        operation; /**< 地址固定的底层异步操作，由当前包装负责启动、等待和交付结果。 */

    /** @brief 按具体调度模式构造地址固定的 sender 操作。 */
    implementation(faio::runtime::detail::runtime_context& context,
                   Sender sender,
                   Receiver receiver)
        : operation(context, std::move(sender), std::move(receiver)) {}

    /** @brief 按执行协议启动已连接的操作，此后只能交付一次终态。 */
    void start() noexcept override { operation.start(); }

    /** @brief 返回关联任务图的根状态租约。 */
    auto root() const noexcept -> root_handle override { return operation.root(); }
  };

 public:
  using operation_state_concept =
      stdexec::operation_state_tag; /**< 标记该类型为操作状态，连接后其地址必须保持不变。 */

  /** @brief 按运行时模式创建具体的稳定操作。 */
  runtime_bound_operation(faio::runtime::detail::runtime_context& context,
                          Sender sender,
                          Receiver receiver) {
    if (context.config()._mode == faio::runtime::mode::current_thread)
      operation_ = std::make_unique<implementation<faio::runtime::mode::current_thread>>(
          context, std::move(sender), std::move(receiver));
    else
      operation_ = std::make_unique<implementation<faio::runtime::mode::multi_thread>>(
          context, std::move(sender), std::move(receiver));
  }

  /** @brief 禁止复制或移动稳定操作状态。 */
  runtime_bound_operation(const runtime_bound_operation&) = delete;
  /** @brief 禁止复制或移动稳定操作状态。 */
  runtime_bound_operation(runtime_bound_operation&&) = delete;

  /** @brief 按执行协议启动已连接的操作，此后只能交付一次终态。 */
  void start() noexcept { operation_->start(); }

  /** @brief 返回关联任务图的根状态租约。 */
  auto root() const noexcept -> root_handle { return operation_->root(); }

 private:
  std::unique_ptr<base>
      operation_; /**< 地址固定的底层异步操作，由当前包装负责启动、等待和交付结果。 */
};

/** @brief 连接时按运行时模式绑定的 sender。 */
template <class Sender>
class runtime_bound_sender {
 public:
  using sender_concept = stdexec::sender_tag; /**< 标记该类型为 sender/receiver 协议中的发送器。 */

  /** @brief 保存运行时和待连接的源 sender。 */
  runtime_bound_sender(faio::runtime::detail::runtime_context& context, Sender sender)
      : context_(&context), sender_(std::move(sender)) {}

  /** @brief 移交绑定到运行时的 sender。 */
  runtime_bound_sender(runtime_bound_sender&&) = default;
  /** @brief 禁止复制源 sender 的所有权。 */
  runtime_bound_sender(const runtime_bound_sender&) = delete;

  /** @brief 声明 sender 在给定环境中的完成签名。 */
  template <class Self, class Environment = stdexec::env<>>
  static consteval auto get_completion_signatures() noexcept {
    using current = stdexec::completion_signatures_of_t<
        Sender,
        root_environment<faio::runtime::mode::current_thread, Environment>>;
    using multi = stdexec::completion_signatures_of_t<
        Sender,
        root_environment<faio::runtime::mode::multi_thread, Environment>>;
    static_assert(std::same_as<current, multi>,
                  "faio experimental: runtime_ref sender signatures depend on runtime mode");
    return current{};
  }

  /** @brief 连接 receiver，创建地址固定的异步操作。 */
  template <stdexec::receiver Receiver>
  auto connect(Receiver receiver) && -> runtime_bound_operation<Sender, Receiver> {
    return {*context_, std::move(sender_), std::move(receiver)};
  }

  /** @brief 转发源 sender 的环境查询。 */
  auto get_env() const noexcept { return stdexec::get_env(sender_); }

 private:
  faio::runtime::detail::runtime_context*
      context_;   /**< 借用运行时上下文，运行时必须在整个操作期间保持存活。 */
  Sender sender_; /**< 尚未连接或启动的源发送器对象。 */
};
}  // namespace faio::experimental::detail

namespace faio::experimental {
/** @brief 将 sender 绑定到目标运行时及其任务图。 */
template <class Sender>
auto runtime_ref::bind(Sender&& sender) const {
  return detail::runtime_bound_sender<std::decay_t<Sender>>{context(),
                                                            std::forward<Sender>(sender)};
}
}  // namespace faio::experimental
#endif
