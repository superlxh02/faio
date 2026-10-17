/**
 * @file faio/detail/experimental/execution/run.hpp
 * @brief 实验性的入口注解与发送器接收器适配：发送器同步运行与排空。
 * @details 实验适配在编译期开关下启用，连接阶段建立稳定操作状态；启动、结果交付与关闭沿受控任务图的租约和排空协议推进。
 */
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
 private: Error error_;  ///< 保存的操作错误，供完成或重复查询返回。
};
namespace detail {
template <class... Tuples> struct one_value_tuple {
  static_assert(sizeof...(Tuples) == 1,
                "faio experimental: run requires one value tuple; use into_variant to normalize signatures");
};
template <class Tuple> struct one_value_tuple<Tuple> {
  using type = Tuple;  ///< 当前类型变换得到的目标类型。
};
template <class... Tuples> using single_value_tuple = typename one_value_tuple<Tuples...>::type;
template <class... Values> using decayed_tuple = std::tuple<std::decay_t<Values>...>;
template <class Sender>
using result_tuple = stdexec::value_types_of_t<Sender, stdexec::env<>, decayed_tuple, single_value_tuple>;

template <class Tuple> struct execution_result {
  std::optional<Tuple> value;  ///< 持有当前结果值或快照，读取前须确认操作已完成。
  std::exception_ptr error;  ///< 跨异步边界保存的异常，结果领取时重新抛出。
  std::atomic<bool> done{false};  ///< 操作是否已结束，后续推进据此直接返回。
  external_host* host;  ///< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。
  explicit execution_result(external_host& target) noexcept : host(&target) {}
  /// @brief 发布同步等待结果已完成，并通知原子等待者与宿主排空检查。
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
  using receiver_concept = stdexec::receiver_tag;  ///< 标记该类型符合发送器接收器协议中的接收器角色。
  execution_result<Tuple>* result;  ///< 已完成操作的返回值；具体错误解释遵循所属接口约定。
  /// @brief 返回接收器环境视图，传播调度归属、停止令牌及原环境查询。
  auto get_env() const noexcept { return stdexec::env<>{}; }
  /// @brief 向接收器交付成功值，交付后不再访问可被回调销毁的操作。
  template <class... Values> void set_value(Values&&... values) && noexcept {
    try { result->value.emplace(std::forward<Values>(values)...); }
    catch (...) { result->error = std::current_exception(); }
    result->finish();
  }
  /// @brief 向接收器交付失败，交付后不再访问可被回调销毁的操作。
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
  /// @brief 向接收器交付协作停止终态，归还任务图完成责任。
  void set_stopped() && noexcept { result->finish(); }
};
}

template <class Sender> auto runtime_ref::run(Sender&& sender) const {
  if (faio::detail::on_runtime_worker())
    throw std::logic_error("faio experimental: run cannot block a runtime worker");
  auto bound = bind(std::forward<Sender>(sender));
  using tuple = detail::result_tuple<decltype(bound)>;
  detail::execution_result<tuple> result{context().external_host()};
  auto operation = stdexec::connect(std::move(bound), detail::result_receiver<tuple>{&result});
  /// @brief 元数据保存在操作对象之外；接收器可销毁操作，而根任务独立的分派与发布守卫仍继续排空。
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
}
#endif
