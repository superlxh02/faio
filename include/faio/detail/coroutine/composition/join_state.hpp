/**
 * @file faio/detail/coroutine/composition/join_state.hpp
 * @brief 根协程包装器与结果句柄共享的完成状态。
 */
#pragma once

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/composition/result.hpp"
#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/coroutine/wait/completion_event.hpp"
#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace faio::detail {
/**
 * @brief 将父停止源的请求转发给子停止源，不销毁任务。
 * 实际取消由正在等待的 operation 处理。
 */
struct forward_stop {
  std::stop_source* source; /**< 借用目标停止源；所属共享状态或作用域保证它在回调注销前一直有效。 */

  /** @brief 父令牌停止时向目标源发出同样的协作请求。 */
  void operator()() const noexcept { source->request_stop(); }
};

/**
 * @brief 根协程与唯一结果句柄共同持有的完成状态。
 * 结果发布后不再修改 value/error，ready 和 completion 建立可见性。
 */
template <class T, class S = default_scheduler>
struct join_handle_state {
  std::optional<handle_value<T>> value; /**< 正常完成的结果；void 使用占位值，失败时保持为空。 */
  std::exception_ptr error;             /**< 用户 task 抛出的异常，消费结果时重新抛出。 */
  ::faio::detail::basic_completion_event<S>
      completion;                 /**< 供协程等待者使用的完成事件，不阻塞 worker。 */
  std::atomic<bool> ready{false}; /**< 供普通线程 done/wait 使用的完成标志，发布后不复位。 */
  std::atomic<bool> blocking_waiter{
      false}; /**< 普通线程 wait/get 的登记标志，设置后不复位；协程等待者使用 completion，无需唤醒原子等待桶。 */
  std::atomic<bool> consumed{
      false}; /**< 结果是否已经领取，包含已观察并重抛异常的情况，限制单次消费。 */
  std::atomic<bool> abandoned{false}; /**< 消费权是否已被丢弃，配合发布路径处理无人观察的异常。 */
  std::stop_source stop_source;       /**< 子任务独立停止源，request_stop 与父停止转发都操作它。 */
  std::optional<std::stop_callback<forward_stop>> parent_callback{
      std::nullopt}; /**< 可选父停止回调，保存在共享状态内以覆盖整个子任务生命周期。 */
  cancellation_error_policy cancellation_policy{
      current_task_context()
          .cancellation_policy}; /**< 未观察的取消异常处理策略，随任务上下文传播。 */
  std::shared_ptr<cancellation_state>
      cancellation_owner;  /**< 持有取消回调链的共享状态，不持有协程结果或协程帧。 */
  bool normal_cancelled{}; /**< 标记取消已按正常结束处理，领取结果时据此区分失败。 */

  /** @brief 保存任务失败信息。 */
  void capture_error(std::exception_ptr failure) noexcept {
    error = std::move(failure);
    try {
      if (error)
        std::rethrow_exception(error);
    } catch (const operation_cancelled&) {
      normal_cancelled = cancellation_policy == cancellation_error_policy::NORMAL_IF_STOP_REQUESTED
                         && stop_source.stop_requested();
    } catch (...) {
      normal_cancelled = false;
    }
  }

  /** @brief 写入结果或异常后发布完成状态，同时唤醒普通线程与协程等待者。 */
  void publish() {
    ready.store(true, std::memory_order_seq_cst);
    if (blocking_waiter.load(std::memory_order_seq_cst))
      ready.notify_all();
    completion.notify();
    if (abandoned.load(std::memory_order_seq_cst) && error && !normal_cancelled)
      std::terminate();
  }

  /** @brief 放弃唯一消费权；不会请求停止，也不会立刻销毁运行中的任务。 */
  void abandon() noexcept {
    if (consumed.load(std::memory_order_acquire))
      return;
    abandoned.store(true, std::memory_order_seq_cst);
    if (ready.load(std::memory_order_seq_cst) && error && !normal_cancelled)
      std::terminate();
  }

  /** @brief 领取一次结果或观察异常；调用前必须已通过 wait/完成事件确认任务完成。 */
  T take() {
    if (consumed.exchange(true, std::memory_order_acq_rel))
      throw std::logic_error("JoinHandle 结果只能消费一次");
    if (error)
      std::rethrow_exception(error);
    if constexpr (!std::is_void_v<T>)
      return std::move(value.value());
  }
};

}  // namespace faio::detail
