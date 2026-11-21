/**
 * @file faio/detail/coroutine/awaiter/state.hpp
 * @brief 异步操作的稳定状态及完成发布期间的生命周期责任。
 */
#ifndef FAIO_DETAIL_COROUTINE_AWAITER_STATE_HPP
#define FAIO_DETAIL_COROUTINE_AWAITER_STATE_HPP

#include "faio/detail/coroutine/lifetime/reference.hpp"
#include "faio/detail/coroutine/lifetime/tracker.hpp"
#include "faio/detail/coroutine/context/execution_binding.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/execution_adapter/child_lifetime.hpp"
#endif
#include <atomic>
#include <stop_token>
#include <utility>

namespace faio::detail {
/**
 * @brief 维持外部完成发布、操作析构和调度器入队这几个收尾步骤所需的生命周期。
 * 必须先销毁 Operation，再释放此生命周期责任；未开始的操作无需登记。
 */
class custom_awaiter_lifetime
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    : private execution_child_lifetime
#endif
{
 public:
  void capture(task_tracker* scope,
               task_lifetime_ref lifetime
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
               ,
               external_scope_ref external
#endif
               ) noexcept {
    scope_ = scope;
    lifetime_ = lifetime;
    if (scope_)
      scope_->add();
    lifetime_.register_task();
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    capture_execution_child(external);
#endif
  }

  /** @brief 创建尚未登记的生命周期对象。 */
  custom_awaiter_lifetime() = default;
  /** @brief 禁止复制生命周期责任。 */
  custom_awaiter_lifetime(const custom_awaiter_lifetime&) = delete;
  /** @brief 禁止复制赋值。 */
  custom_awaiter_lifetime& operator=(const custom_awaiter_lifetime&) = delete;

  /** @brief 销毁自定义等待器生命周期租约。 */
  ~custom_awaiter_lifetime() {
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    auto completion = scope_ ? scope_->done() : task_tracker::completion_guard{};
#else
    if (scope_)
      scope_->done();
#endif
    lifetime_.finish_task();
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    finish_execution_child();
#endif
  }

 private:
  task_tracker* scope_{};
  task_lifetime_ref lifetime_{};
};

/** @brief 状态不依赖调度器类型；重绑定只转交所有权，既不移动 Operation，也不创建通知帧。 */
template <class Operation>
struct custom_awaiter_state {
  enum phase : unsigned char { idle, registering, suspended, completed };

  /** @brief 构造自定义等待器的结果与生命周期状态。 */
  template <class O>
  explicit custom_awaiter_state(O&& value) : operation(std::forward<O>(value)) {}

  custom_awaiter_lifetime lifetime; /**< 最后析构，确保 operation 清理期间仍然有效。 */
  Operation operation;
  std::stop_token stop;
  std::atomic<unsigned char> gate{idle};
};

}  // namespace faio::detail

#endif
