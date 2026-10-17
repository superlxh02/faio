/**
 * @file faio/detail/experimental/execution/bind_sender.hpp
 * @brief 实验性的入口注解与发送器接收器适配：发送器运行时绑定。
 * @details 实验适配在编译期开关下启用，连接阶段建立稳定操作状态；启动、结果交付与关闭沿受控任务图的租约和排空协议推进。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_BIND_SENDER_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_BIND_SENDER_HPP
#include "faio/detail/experimental/execution/environment.hpp"
#include <optional>

namespace faio::experimental::detail {
template <faio::runtime::mode Mode, class Sender, class Receiver>
class bound_operation {
  struct forwarding_receiver {
    using receiver_concept = stdexec::receiver_tag;  ///< 标记该类型符合发送器接收器协议中的接收器角色。
    bound_operation* owner;  ///< 借用所属对象的稳定地址，回调或等待期间不得迁移或销毁。
    /// @brief 返回接收器环境视图，传播调度归属、停止令牌及原环境查询。
    auto get_env() const noexcept -> root_environment<Mode, stdexec::env_of_t<Receiver>> {
      return root_environment<Mode, stdexec::env_of_t<Receiver>>{
          owner->scheduler_, owner->root_, stdexec::get_env(owner->receiver_)};
    }
    /// @brief 向接收器交付成功值，交付后不再访问可被回调销毁的操作。
    template <class... Values> void set_value(Values&&... values) && noexcept {
      auto root = owner->root_;
      /// @brief 完成可来自其他调度器；即使本宿主没有分派凭证，结果发布期间也必须保持任务图活跃。
      faio::detail::external_publisher_guard publisher{root};
      owner->terminal_ = true;
      auto receiver = std::move(owner->receiver_);
      root->terminal();
      stdexec::set_value(std::move(receiver), std::forward<Values>(values)...);
    }
    /// @brief 向接收器交付失败，交付后不再访问可被回调销毁的操作。
    template <class Error> void set_error(Error&& error) && noexcept {
      auto root = owner->root_;
      faio::detail::external_publisher_guard publisher{root};
      owner->terminal_ = true;
      auto receiver = std::move(owner->receiver_);
      root->terminal();
      stdexec::set_error(std::move(receiver), std::forward<Error>(error));
    }
    /// @brief 向接收器交付协作停止终态，归还任务图完成责任。
    void set_stopped() && noexcept {
      auto root = owner->root_;
      faio::detail::external_publisher_guard publisher{root};
      owner->terminal_ = true;
      auto receiver = std::move(owner->receiver_);
      root->terminal();
      stdexec::set_stopped(std::move(receiver));
    }
  };
  using token_type = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;  ///< 从接收器环境推导的停止令牌类型。
  using started_sender = decltype(stdexec::starts_on(std::declval<execution_scheduler<Mode>>(), std::declval<Sender>()));  ///< 绑定目标调度器后实际连接到接收器的发送器类型。
  using operation_type = stdexec::connect_result_t<started_sender, forwarding_receiver>;  ///< 连接发送器与接收器后得到的稳定操作状态类型。
 public:
  using operation_state_concept = stdexec::operation_state_tag;  ///< 标记该类型为连接后地址稳定的操作状态。
  bound_operation(faio::runtime::detail::runtime_context& context, Sender sender, Receiver receiver)
      : receiver_(std::move(receiver)), root_(context.external_host().acquire_root()),
        scheduler_(context, root_), reservation_(context.external_host().reserve(root_, true)),
        stop_(stdexec::get_stop_token(stdexec::get_env(receiver_)), request_root_stop{root_.get()}),
        operation_(stdexec::connect(stdexec::starts_on(scheduler_, std::move(sender)), forwarding_receiver{this})) {}
  bound_operation(const bound_operation&) = delete;
  bound_operation(bound_operation&&) = delete;
  ~bound_operation() {
    if (started_ && !terminal_) std::terminate();
    if (!started_) { reservation_.reset(); root_->terminal(); }
  }
  /// @brief 把已连接的稳定操作交给其执行协议，之后只能交付一个终态。
  void start() noexcept {
    if (std::exchange(started_, true)) std::terminate();
    reservation_.start();
    stdexec::start(operation_);
  }
  auto root() const noexcept -> const root_handle& { return root_; }
 private:
  Receiver receiver_;  ///< 结果交付接收器，完成时移出以允许回调销毁当前操作。
  root_handle root_;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
  execution_scheduler<Mode> scheduler_;  ///< 捕获的目标调度器，跨线程完成时将续体投递回所属运行时。
  faio::detail::external_reservation reservation_;  ///< 启动前保留的任务图接纳租约，发布成功后转交执行责任。
  stop_callback<token_type, request_root_stop> stop_;  ///< 将外部停止请求转发到本根任务的回调连接。
  bool started_{};  ///< 标记操作已启动，防止重复启动或误退还未启动预留。
  bool terminal_{};  ///< 标记已交付终态，析构据此区分放弃与正常结束。
  operation_type operation_;  ///< 地址稳定的底层异步操作，当前包装负责其启动、等待与结果交付。
};

template <faio::runtime::mode Mode, class Sender>
class bound_sender {
 public:
  using sender_concept = stdexec::sender_tag;  ///< 标记该类型符合发送器接收器协议中的发送器角色。
  bound_sender(faio::runtime::detail::runtime_context& context, Sender sender)
      : context_(&context), sender_(std::move(sender)) {}
  bound_sender(bound_sender&&) = default;
  bound_sender(const bound_sender&) = delete;
  template <class Self, class Environment = stdexec::env<>>
  static consteval auto get_completion_signatures() noexcept {
    return stdexec::completion_signatures_of_t<Sender, root_environment<Mode, Environment>>{};
  }
  template <stdexec::receiver Receiver>
  auto connect(Receiver receiver) && -> bound_operation<Mode, Sender, Receiver> {
    return {*context_, std::move(sender_), std::move(receiver)};
  }
  /// @brief 返回接收器环境视图，传播调度归属、停止令牌及原环境查询。
  auto get_env() const noexcept { return stdexec::get_env(sender_); }
 private:
  faio::runtime::detail::runtime_context* context_;  ///< 借用运行时上下文，其生命期必须覆盖本操作。
  Sender sender_;  ///< 尚未连接或启动的源发送器对象。
};

/// @brief runtime_ref 在运行期选择模式；类型擦除仅发生在连接阶段，启动与完成期间不分配或迁移稳定内部操作。
template <class Sender, class Receiver>
class runtime_bound_operation {
  struct base {
    virtual ~base() = default;
    virtual void start() noexcept = 0;
    virtual auto root() const noexcept -> root_handle = 0;
  };
  template <faio::runtime::mode Mode> struct implementation final : base {
    bound_operation<Mode, Sender, Receiver> operation;  ///< 地址稳定的底层异步操作，当前包装负责其启动、等待与结果交付。
    implementation(faio::runtime::detail::runtime_context& context, Sender sender, Receiver receiver)
        : operation(context, std::move(sender), std::move(receiver)) {}
    /// @brief 把已连接的稳定操作交给其执行协议，之后只能交付一个终态。
    void start() noexcept override { operation.start(); }
    auto root() const noexcept -> root_handle override { return operation.root(); }
  };
 public:
  using operation_state_concept = stdexec::operation_state_tag;  ///< 标记该类型为连接后地址稳定的操作状态。
  runtime_bound_operation(faio::runtime::detail::runtime_context& context, Sender sender, Receiver receiver) {
    if (context.config()._mode == faio::runtime::mode::current_thread)
      operation_ = std::make_unique<implementation<faio::runtime::mode::current_thread>>(context, std::move(sender), std::move(receiver));
    else
      operation_ = std::make_unique<implementation<faio::runtime::mode::multi_thread>>(context, std::move(sender), std::move(receiver));
  }
  runtime_bound_operation(const runtime_bound_operation&) = delete;
  runtime_bound_operation(runtime_bound_operation&&) = delete;
  /// @brief 把已连接的稳定操作交给其执行协议，之后只能交付一个终态。
  void start() noexcept { operation_->start(); }
  auto root() const noexcept -> root_handle { return operation_->root(); }
 private:
  std::unique_ptr<base> operation_;  ///< 地址稳定的底层异步操作，当前包装负责其启动、等待与结果交付。
};
template <class Sender>
class runtime_bound_sender {
 public:
  using sender_concept = stdexec::sender_tag;  ///< 标记该类型符合发送器接收器协议中的发送器角色。
  runtime_bound_sender(faio::runtime::detail::runtime_context& context, Sender sender)
      : context_(&context), sender_(std::move(sender)) {}
  runtime_bound_sender(runtime_bound_sender&&) = default;
  runtime_bound_sender(const runtime_bound_sender&) = delete;
  template <class Self, class Environment = stdexec::env<>>
  static consteval auto get_completion_signatures() noexcept {
    using current = stdexec::completion_signatures_of_t<Sender, root_environment<faio::runtime::mode::current_thread, Environment>>;
    using multi = stdexec::completion_signatures_of_t<Sender, root_environment<faio::runtime::mode::multi_thread, Environment>>;
    static_assert(std::same_as<current, multi>, "faio experimental: runtime_ref sender signatures depend on runtime mode");
    return current{};
  }
  template <stdexec::receiver Receiver>
  auto connect(Receiver receiver) && -> runtime_bound_operation<Sender, Receiver> {
    return {*context_, std::move(sender_), std::move(receiver)};
  }
  /// @brief 返回接收器环境视图，传播调度归属、停止令牌及原环境查询。
  auto get_env() const noexcept { return stdexec::get_env(sender_); }
 private:
  faio::runtime::detail::runtime_context* context_;  ///< 借用运行时上下文，其生命期必须覆盖本操作。
  Sender sender_;  ///< 尚未连接或启动的源发送器对象。
};
}

namespace faio::experimental {
template <class Sender> auto runtime_ref::bind(Sender&& sender) const {
  return detail::runtime_bound_sender<std::decay_t<Sender>>{context(), std::forward<Sender>(sender)};
}
}
#endif
