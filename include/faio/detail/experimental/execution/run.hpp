/**
 * @file faio/detail/experimental/execution/run.hpp
 * @brief 发送器同步运行与排空。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_RUN_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_RUN_HPP
#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/experimental/execution/bind_sender.hpp"
#include <atomic>
#include <exception>
#include <optional>
#include <system_error>
#include <tuple>

namespace faio::experimental {
/** @brief 包装 sender 返回的非 exception_ptr 错误值。 */
template <class Error>
class execution_error : public std::runtime_error {
 public:
  /** @brief 保存 sender 的原始错误值。 */
  explicit execution_error(Error error)
      : std::runtime_error("faio experimental: sender error"), error_(std::move(error)) {}

  /** @brief 返回 sender 携带的原始错误值。 */
  const Error& error() const noexcept { return error_; }

 private:
  Error error_; /**< 保存的操作错误，供完成或重复查询返回。 */
};

namespace detail {
/** @brief 要求 sender 恰好有一种值完成签名。 */
template <class... Tuples>
struct one_value_tuple {
  static_assert(
      sizeof...(Tuples) == 1,
      "faio experimental: run requires one value tuple; use into_variant to normalize signatures");
};

/** @brief 提取 sender 唯一的值元组类型。 */
template <class Tuple>
struct one_value_tuple<Tuple> {
  using type = Tuple; /**< sender 唯一的值元组类型。 */
};
template <class... Tuples>
using single_value_tuple = typename one_value_tuple<Tuples...>::type;
template <class... Values>
using decayed_tuple = std::tuple<std::decay_t<Values>...>;
template <class Sender>
using result_tuple =
    stdexec::value_types_of_t<Sender, stdexec::env<>, decayed_tuple, single_value_tuple>;

/** @brief 保存 sender 的终态，并通知同步等待者。 */
template <class Tuple>
struct execution_result {
  std::optional<Tuple> value;    /**< 保存 sender 的结果值，读取前须确认操作已完成。 */
  std::exception_ptr error;      /**< 跨异步边界保存异常，在领取结果时重新抛出。 */
  std::atomic<bool> done{false}; /**< 标记 sender 是否已结束。 */
  external_host* host;           /**< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。 */

  /** @brief 关联完成通知所需的外部任务宿主。 */
  explicit execution_result(external_host& target) noexcept : host(&target) {}

  /** @brief 发布同步等待结果的完成状态，并通知原子等待者和宿主检查排空状态。 */
  void finish() noexcept {
    done.store(true, std::memory_order_release);
    done.notify_all();
    host->notify_completion();
  }

  /** @brief 领取 sender 的值，或重新抛出已保存的异常。 */
  auto take() -> std::optional<Tuple> {
    if (error)
      std::rethrow_exception(error);
    return std::move(value);
  }
};

/** @brief 将 sender 的终态写入共享执行结果。 */
template <class Tuple>
struct result_receiver {
  using receiver_concept =
      stdexec::receiver_tag;       /**< 标记该类型为 sender/receiver 协议中的接收器。 */
  execution_result<Tuple>* result; /**< 借用共享执行结果，用于保存 sender 的终态。 */

  /** @brief 返回同步结果接收器的空环境。 */
  auto get_env() const noexcept { return stdexec::env<>{}; }

  /** @brief 向接收器交付成功值，交付后不再访问可被回调销毁的操作。 */
  template <class... Values>
  void set_value(Values&&... values) && noexcept {
    try {
      result->value.emplace(std::forward<Values>(values)...);
    } catch (...) {
      result->error = std::current_exception();
    }
    result->finish();
  }

  /** @brief 向接收器交付失败，交付后不再访问可被回调销毁的操作。 */
  template <class Error>
  void set_error(Error&& error) && noexcept {
    try {
      if constexpr (std::same_as<std::decay_t<Error>, std::exception_ptr>)
        result->error = std::forward<Error>(error);
      else if constexpr (std::same_as<std::decay_t<Error>, std::error_code>)
        throw std::system_error(std::forward<Error>(error));
      else
        throw execution_error<std::decay_t<Error>>(std::forward<Error>(error));
    } catch (...) {
      result->error = std::current_exception();
    }
    result->finish();
  }

  /** @brief 记录停止终态，并通知同步等待者。 */
  void set_stopped() && noexcept { result->finish(); }
};
}  // namespace detail

/** @brief 同步驱动 sender，待所属任务图排空后领取结果。 */
template <class Sender>
auto runtime_ref::run(Sender&& sender) const {
  if (faio::detail::on_runtime_worker())
    throw std::logic_error("faio experimental: run cannot block a runtime worker");
  auto bound = bind(std::forward<Sender>(sender));
  using tuple = detail::result_tuple<decltype(bound)>;
  detail::execution_result<tuple> result{context().external_host()};
  auto operation = stdexec::connect(std::move(bound), detail::result_receiver<tuple>{&result});
  auto root = operation.root();
  stdexec::start(operation);
  context().drive_until(
      [&] { return result.done.load(std::memory_order_acquire) && root->drained(); });
  return result.take();
}

/** @brief 取得当前 faio 执行线程所属的运行时；未绑定时抛出异常。 */
inline auto this_runtime() -> runtime_ref {
  auto* host = ::faio::detail::current_execution_binding()
                   ? ::faio::detail::current_execution_binding()->external_host()
                   : nullptr;
  if (!host || !host->context())
    throw std::logic_error(
        "faio experimental: this_runtime requires an active faio execution thread");
  return runtime_ref{*host->context()};
}
}  // namespace faio::experimental
#endif
