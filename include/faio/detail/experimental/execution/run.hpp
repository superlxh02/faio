#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_RUN_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_RUN_HPP
#include "faio/detail/experimental/execution/bind_sender.hpp"
#include <atomic>
#include <exception>
#include <optional>
#include <system_error>
#include <tuple>

namespace faio::experimental {
template <class Error> class execution_error : public std::runtime_error {
 public:
  explicit execution_error(Error error)
      : std::runtime_error("faio experimental: sender error"), error_(std::move(error)) {}
  const Error& error() const noexcept { return error_; }
 private: Error error_;
};
namespace detail {
template <class... Tuples> struct one_value_tuple {
  static_assert(sizeof...(Tuples) == 1,
                "faio experimental: run requires one value tuple; use into_variant to normalize signatures");
};
template <class Tuple> struct one_value_tuple<Tuple> { using type = Tuple; };
template <class... Tuples> using single_value_tuple = typename one_value_tuple<Tuples...>::type;
template <class... Values> using decayed_tuple = std::tuple<std::decay_t<Values>...>;
template <class Sender>
using result_tuple = stdexec::value_types_of_t<Sender, stdexec::env<>, decayed_tuple, single_value_tuple>;

template <class Tuple> struct execution_result {
  std::optional<Tuple> value;
  std::exception_ptr error;
  std::atomic<bool> done{false};
  external_host* host;
  explicit execution_result(external_host& target) noexcept : host(&target) {}
  void finish() noexcept {
    done.store(true, std::memory_order_release);
    done.notify_all();
    host->notify_completion();
  }
  auto take() -> std::optional<Tuple> {
    if (error) std::rethrow_exception(error);
    return std::move(value);
  }
};
template <class Tuple> struct result_receiver {
  using receiver_concept = stdexec::receiver_tag;
  execution_result<Tuple>* result;
  auto get_env() const noexcept { return stdexec::env<>{}; }
  template <class... Values> void set_value(Values&&... values) && noexcept {
    try { result->value.emplace(std::forward<Values>(values)...); }
    catch (...) { result->error = std::current_exception(); }
    result->finish();
  }
  template <class Error> void set_error(Error&& error) && noexcept {
    try {
      if constexpr (std::same_as<std::decay_t<Error>, std::exception_ptr>)
        result->error = std::forward<Error>(error);
      else if constexpr (std::same_as<std::decay_t<Error>, std::error_code>)
        throw std::system_error(std::forward<Error>(error));
      else
        throw execution_error<std::decay_t<Error>>(std::forward<Error>(error));
    } catch (...) { result->error = std::current_exception(); }
    result->finish();
  }
  void set_stopped() && noexcept { result->finish(); }
};
} // namespace detail

template <class Sender> auto runtime_ref::run(Sender&& sender) const {
  if (faio::detail::on_runtime_worker())
    throw std::logic_error("faio experimental: run cannot block a runtime worker");
  auto bound = bind(std::forward<Sender>(sender));
  using tuple = detail::result_tuple<decltype(bound)>;
  detail::execution_result<tuple> result{context().external_host()};
  auto operation = stdexec::connect(std::move(bound), detail::result_receiver<tuple>{&result});
  // Keep metadata outside operation: a receiver is allowed to destroy an
  // operation, while the root's independent dispatch/publisher guards drain.
  auto root = operation.root();
  stdexec::start(operation);
  context().drive_until([&] { return result.done.load(std::memory_order_acquire) && root->drained(); });
  return result.take();
}
inline auto this_runtime() -> runtime_ref {
  auto* host = faio::detail::current_execution_thread ? faio::detail::current_execution_thread->external_host() : nullptr;
  if (!host || !host->context())
    throw std::logic_error("faio experimental: this_runtime requires an active faio execution thread");
  return runtime_ref{*host->context()};
}
} // namespace faio::experimental
#endif
