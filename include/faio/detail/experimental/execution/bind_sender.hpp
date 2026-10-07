#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_BIND_SENDER_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_BIND_SENDER_HPP
#include "faio/detail/experimental/execution/environment.hpp"
#include <optional>

namespace faio::experimental::detail {
template <faio::runtime::mode Mode, class Sender, class Receiver>
class bound_operation {
  struct forwarding_receiver {
    using receiver_concept = stdexec::receiver_tag;
    bound_operation* owner;
    auto get_env() const noexcept -> root_environment<Mode, stdexec::env_of_t<Receiver>> {
      return root_environment<Mode, stdexec::env_of_t<Receiver>>{
          owner->scheduler_, owner->root_, stdexec::get_env(owner->receiver_)};
    }
    template <class... Values> void set_value(Values&&... values) && noexcept {
      auto root = owner->root_;
      // Completion can arrive from another scheduler. Keep the graph active
      // through result publication even without a dispatch ticket on this host.
      faio::detail::external_publisher_guard publisher{root};
      owner->terminal_ = true;
      auto receiver = std::move(owner->receiver_);
      root->terminal();
      stdexec::set_value(std::move(receiver), std::forward<Values>(values)...);
    }
    template <class Error> void set_error(Error&& error) && noexcept {
      auto root = owner->root_;
      faio::detail::external_publisher_guard publisher{root};
      owner->terminal_ = true;
      auto receiver = std::move(owner->receiver_);
      root->terminal();
      stdexec::set_error(std::move(receiver), std::forward<Error>(error));
    }
    void set_stopped() && noexcept {
      auto root = owner->root_;
      faio::detail::external_publisher_guard publisher{root};
      owner->terminal_ = true;
      auto receiver = std::move(owner->receiver_);
      root->terminal();
      stdexec::set_stopped(std::move(receiver));
    }
  };
  using token_type = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
  using started_sender = decltype(stdexec::starts_on(std::declval<execution_scheduler<Mode>>(), std::declval<Sender>()));
  using operation_type = stdexec::connect_result_t<started_sender, forwarding_receiver>;
 public:
  using operation_state_concept = stdexec::operation_state_tag;
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
  void start() noexcept {
    if (std::exchange(started_, true)) std::terminate();
    reservation_.start();
    stdexec::start(operation_);
  }
  auto root() const noexcept -> const root_handle& { return root_; }
 private:
  Receiver receiver_;
  root_handle root_;
  execution_scheduler<Mode> scheduler_;
  faio::detail::external_reservation reservation_;
  stop_callback<token_type, request_root_stop> stop_;
  bool started_{};
  bool terminal_{};
  operation_type operation_;
};

template <faio::runtime::mode Mode, class Sender>
class bound_sender {
 public:
  using sender_concept = stdexec::sender_tag;
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
  auto get_env() const noexcept { return stdexec::get_env(sender_); }
 private:
  faio::runtime::detail::runtime_context* context_;
  Sender sender_;
};

// runtime_ref chooses its mode at run time. Type erasure is confined to the
// connect phase; the stable inner operation is never relocated or allocated
// during start/completion.
template <class Sender, class Receiver>
class runtime_bound_operation {
  struct base {
    virtual ~base() = default;
    virtual void start() noexcept = 0;
    virtual auto root() const noexcept -> root_handle = 0;
  };
  template <faio::runtime::mode Mode> struct implementation final : base {
    bound_operation<Mode, Sender, Receiver> operation;
    implementation(faio::runtime::detail::runtime_context& context, Sender sender, Receiver receiver)
        : operation(context, std::move(sender), std::move(receiver)) {}
    void start() noexcept override { operation.start(); }
    auto root() const noexcept -> root_handle override { return operation.root(); }
  };
 public:
  using operation_state_concept = stdexec::operation_state_tag;
  runtime_bound_operation(faio::runtime::detail::runtime_context& context, Sender sender, Receiver receiver) {
    if (context.config()._mode == faio::runtime::mode::current_thread)
      operation_ = std::make_unique<implementation<faio::runtime::mode::current_thread>>(context, std::move(sender), std::move(receiver));
    else
      operation_ = std::make_unique<implementation<faio::runtime::mode::multi_thread>>(context, std::move(sender), std::move(receiver));
  }
  runtime_bound_operation(const runtime_bound_operation&) = delete;
  runtime_bound_operation(runtime_bound_operation&&) = delete;
  void start() noexcept { operation_->start(); }
  auto root() const noexcept -> root_handle { return operation_->root(); }
 private:
  std::unique_ptr<base> operation_;
};
template <class Sender>
class runtime_bound_sender {
 public:
  using sender_concept = stdexec::sender_tag;
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
  auto get_env() const noexcept { return stdexec::get_env(sender_); }
 private:
  faio::runtime::detail::runtime_context* context_;
  Sender sender_;
};
} // namespace faio::experimental::detail

namespace faio::experimental {
template <class Sender> auto runtime_ref::bind(Sender&& sender) const {
  return detail::runtime_bound_sender<std::decay_t<Sender>>{context(), std::forward<Sender>(sender)};
}
} // namespace faio::experimental
#endif
