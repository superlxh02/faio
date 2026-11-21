/**
 * @file faio/detail/coroutine/awaiter/operation.hpp
 * @brief 用户异步操作的两阶段静态协议约束。
 */
#ifndef FAIO_DETAIL_COROUTINE_AWAITER_OPERATION_HPP
#define FAIO_DETAIL_COROUTINE_AWAITER_OPERATION_HPP

#include "faio/detail/coroutine/scheduler/reference.hpp"
#include <concepts>
#include <type_traits>
#include <utility>

namespace faio {
namespace detail {
/** @brief 自定义等待器的结果与生命周期状态。 */
template <class Operation>
struct custom_awaiter_state;

/** @brief 自定义等待器的完成通知入口。 */
template <class Operation, class S>
class awaiter_completion;
}  // namespace detail

/**
 * @brief 用户操作负责启动和保存结果；完成令牌只通知，不保存另一份结果。
 * on_suspend 必须为 noexcept，并恰好通知一次完成；on_resume 可返回 void、值或抛出异常。
 */
template <class Operation, class Completion>
concept await_operation_for =
    std::move_constructible<Operation> && requires(Operation& operation, Completion completion) {
      { operation.on_suspend(std::move(completion)) } noexcept -> std::same_as<void>;
      std::move(operation).on_resume();
      requires !std::is_reference_v<decltype(std::move(operation).on_resume())>;
    };

template <class Operation>
concept await_operation = await_operation_for<
    std::remove_cvref_t<Operation>,
    detail::awaiter_completion<std::remove_cvref_t<Operation>, default_scheduler>>;

}  // namespace faio

#endif
