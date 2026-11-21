/**
 * @file faio/detail/coroutine/composition/when_all.hpp
 * @brief 等待全部任务完成的 join 与 join_all 实现。
 */
#pragma once

#include "faio/detail/coroutine/composition/result.hpp"
#include "faio/detail/coroutine/wait/completion_event.hpp"
#include "faio/detail/coroutine/launch/root_task.hpp"
#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace faio {
namespace detail {
/** @brief 单个分支在协程帧内的结果槽，父任务仅在所有子任务退出后读取。 */
template <class T>
struct join_slot {
  std::optional<joined_value<T>> value; /**< 成功分支的结果，void 分支存占位；失败时保持为空。 */
  std::exception_ptr error;             /**< 本分支失败原因，父协程按输入下标取槽时重抛。 */

  /** @brief 消费已完成槽位的结果或重抛异常，调用前必须等待全部分支完成。 */
  joined_value<T> take() {
    if (error)
      std::rethrow_exception(error);
    return std::move(*value);
  }
};

/** @brief 父协程拥有的 join 完成条件；投递哨兵保证分支登记期间不会提前完成。 */
template <class S>
class join_completion {
 public:
  /** @brief 为一个尚未发布的分支登记完成责任。 */
  void register_task() noexcept { remaining_.fetch_add(1, std::memory_order_relaxed); }

  /** @brief 分支完成后归还责任，由最后完成者通知父协程。 */
  void complete_task() {
    if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
      event_.notify();
  }

  /** @brief 撤销未发布分支的登记；调用时父协程仍持有投递哨兵，无需通知。 */
  void rollback_registration() noexcept { remaining_.fetch_sub(1, std::memory_order_acq_rel); }

  /** @brief 全部分支完成投递或回滚后归还唯一投递哨兵，只能调用一次。 */
  void finish_scheduling() { complete_task(); }

  /** @brief 使用投递前预留的恢复目标等待已接纳分支排空。 */
  auto wait(basic_resume_target<S> target) noexcept { return event_.wait(std::move(target)); }

 private:
  std::atomic<std::size_t> remaining_{1};
  basic_completion_event<S> event_;
};

/**
 * @brief 单个 join 分支的根包装：拥有 child，借用父帧的结果槽和完成条件。
 * 父组合器无论成功还是投递失败，都必须排空已提交根任务再离开。
 */
template <class T, class S>
root_task join_root_task_wrapper(task<T, S> child,
                                 join_slot<T>* slot,
                                 join_completion<S>* completion) {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(child);
      slot->value.emplace();
    } else {
      slot->value.emplace(co_await std::move(child));
    }
  } catch (...) {
    slot->error = std::current_exception();
  }
  completion->complete_task();
}

/**
 * @brief 拥有尚未投递的根帧及 join 登记；准备或投递失败时自动撤销该分支。
 * 根帧自身负责归还 scope/lifetime，join 计数由本对象或已发布分支归还。
 */
template <class S>
class prepared_join_task {
 public:
  prepared_join_task(root_task root,
                     join_completion<S>& completion,
                     basic_scheduler_ref<S> scheduler,
                     task_tracker* scope)
      : root_(std::move(root)), completion_(&completion) {
    completion_->register_task();
    if (scope)
      scope->add();
    try {
      target_ = prepare_root_task(root_, scheduler, scope, current_task_lifetime());
    } catch (...) {
      completion_->rollback_registration();
      throw;
    }
  }

  prepared_join_task(prepared_join_task&& other) noexcept
      : root_(std::move(other.root_)),
        target_(std::move(other.target_)),
        completion_(std::exchange(other.completion_, nullptr)) {}

  prepared_join_task(const prepared_join_task&) = delete;
  prepared_join_task& operator=(const prepared_join_task&) = delete;
  prepared_join_task& operator=(prepared_join_task&&) = delete;

  ~prepared_join_task() {
    if (completion_)
      completion_->rollback_registration();
  }

  /** @brief 按已捕获的调度约束立即投递；失败时销毁根帧并保留 join 回滚责任。 */
  void schedule() {
    auto handle = root_.release_handle();
    try {
      target_.schedule(handle);
    } catch (...) {
      handle.destroy();
      throw;
    }
    completion_ = nullptr;
  }

  /** @brief 向批对象转交根帧和 join 完成责任，本对象此后不再清理它们。 */
  std::coroutine_handle<> release_handle() noexcept {
    completion_ = nullptr;
    return root_.release_handle();
  }

 private:
  root_task root_;
  basic_resume_target<S> target_;
  join_completion<S>* completion_;
};

/** @brief 捕获分支上下文并完成根帧准备；返回值在投递前持有帧与计数回滚责任。 */
template <class T, class S>
prepared_join_task<S> prepare_join_task(task<T, S> child,
                                        join_slot<T>& slot,
                                        join_completion<S>& completion,
                                        basic_scheduler_ref<S> scheduler) {
  auto context = capture_task_context();
  auto* scope = context.scope;
  auto root = join_root_task_wrapper(std::move(child), &slot, &completion);
  root.set_context(std::move(context));
  return prepared_join_task<S>{std::move(root), completion, scheduler, scope};
}

/** @brief 准备并立即投递一个 join 分支，失败时撤销本次登记。 */
template <class T, class S>
void schedule_join_task(task<T, S> child, join_slot<T>& slot, join_completion<S>& completion) {
  auto prepared = prepare_join_task(std::move(child), slot, completion, current_scheduler_for<S>());
  prepared.schedule();
}

/** @brief 在所属线程收集普通分支根帧，满批或显式 schedule_pending 时发布。 */
template <class S>
class join_task_batch {
 public:
  join_task_batch(basic_scheduler_ref<S> scheduler, join_completion<S>& completion)
      : scheduler_(scheduler), completion_(completion) {}

  join_task_batch(const join_task_batch&) = delete;
  join_task_batch& operator=(const join_task_batch&) = delete;
  join_task_batch(join_task_batch&&) = delete;
  join_task_batch& operator=(join_task_batch&&) = delete;

  ~join_task_batch() { discard_pending(); }

  /** @brief 接管并准备分支；缓冲区满时先发布上一批，准备失败时保留已备妥前缀。 */
  template <class T>
  void add(task<T, S> child, join_slot<T>& slot) {
    if (count_ == roots_.size())
      schedule_pending();
    auto prepared = prepare_join_task(std::move(child), slot, completion_, scheduler_);
    roots_[count_++] = prepared.release_handle();
  }

  /** @brief 发布所有待投递分支；逐个投递失败时只清理尚未接管的根帧。 */
  void schedule_pending() {
    if (count_ == 0)
      return;
    if (!scheduler_.native_handle()->try_enqueue_local_batch(pending())) {
      try {
        for (auto& root : pending()) {
          scheduler_.schedule(root);
          root = {};
        }
      } catch (...) {
        discard_pending();
        throw;
      }
    }
    count_ = 0;
  }

 private:
  auto pending() noexcept { return std::span{roots_}.first(count_); }

  void discard_pending() noexcept {
    for (auto root : pending()) {
      if (root) {
        root.destroy();
        completion_.rollback_registration();
      }
    }
    count_ = 0;
  }

  basic_scheduler_ref<S> scheduler_;
  join_completion<S>& completion_;
  std::array<std::coroutine_handle<>, 32> roots_;
  std::size_t count_{};
};

/** @brief 仅判断批量投递是否适用，不构造或接管分支。 */
template <class S>
bool can_schedule_join_batch(std::size_t count, basic_scheduler_ref<S> scheduler) {
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  (void)count;
  (void)scheduler;
  return false;
#else
  if constexpr (!std::is_same_v<S, default_scheduler>) {
    return false;
  } else {
    if (count < 8 || !current_execution_binding())
      return false;
    const auto policy = current_task_context().scheduling;
    /** @brief 其他工作线程可能开始窃取先提交的分支时，逐个发布分支。 */
    return scheduler && scheduler.native_handle()->num_workers() == 1 && !policy.no_stealing
           && !policy.binds_core();
  }
#endif
}

/** @brief 将异构输入与对应结果槽配对，选择批量或逐个投递。 */
template <class S, class... Ts, std::size_t... Is>
void schedule_join_tasks(std::tuple<join_slot<Ts>...>& slots,
                         join_completion<S>& completion,
                         std::index_sequence<Is...>,
                         task<Ts, S>... children) {
  if constexpr (std::is_same_v<S, default_scheduler> && sizeof...(Ts) > 1) {
    const auto scheduler = current_scheduler_for<S>();
    if (can_schedule_join_batch(sizeof...(Ts), scheduler)) {
      join_task_batch<S> batch{scheduler, completion};
      try {
        (batch.add(std::move(children), std::get<Is>(slots)), ...);
      } catch (...) {
        /** @brief 构造某个根失败时仍发布已准备前缀，供父协程排空。 */
        batch.schedule_pending();
        throw;
      }
      batch.schedule_pending();
      return;
    }
  }
  (schedule_join_task(std::move(children), std::get<Is>(slots), completion), ...);
}

/** @brief 将同型任务与同位置结果槽配对，选择批量或逐个投递。 */
template <class T, class S>
void schedule_join_tasks(std::vector<task<T, S>>& children,
                         std::span<join_slot<T>> slots,
                         join_completion<S>& completion) {
  if constexpr (std::is_same_v<S, default_scheduler>) {
    const auto scheduler = current_scheduler_for<S>();
    if (can_schedule_join_batch(children.size(), scheduler)) {
      join_task_batch<S> batch{scheduler, completion};
      try {
        for (std::size_t i = 0; i < children.size(); ++i)
          batch.add(std::move(children[i]), slots[i]);
      } catch (...) {
        batch.schedule_pending();
        throw;
      }
      batch.schedule_pending();
      return;
    }
  }
  for (std::size_t i = 0; i < children.size(); ++i)
    schedule_join_task(std::move(children[i]), slots[i], completion);
}

/** @brief 全部完成后按输入位置收集异构结果，第一个失败槽位在此重抛。 */
template <class... Ts, std::size_t... Is>
auto collect_join(std::tuple<join_slot<Ts>...>& slots, std::index_sequence<Is...>) {
  return std::tuple<joined_value<Ts>...>{std::get<Is>(slots).take()...};
}

}  // namespace detail

/**
 * @brief 组合异构任务；调用函数只构造惰性 task，由 co_await 或 block_on 执行时才并发启动所有分支。
 * 全部分支完成后返回 tuple，void 以 std::monostate 占位；若有异常，按参数顺序重抛第一个异常。
 * 分支失败时仍等待全部分支退出，不会自动请求停止其他分支。
 */
namespace detail {
/** @brief 等待任务组完成并汇总结果。 */
template <class S, class... Ts>
task<std::tuple<detail::joined_value<Ts>...>, S> join_impl(task<Ts, S>... children) {
  if constexpr (sizeof...(Ts) == 0)
    co_return {};
  else {
    auto drain_target = ::faio::detail::capture_current_resume_target<S>();
    std::tuple<detail::join_slot<Ts>...> slots;
    detail::join_completion<S> completion;
    std::exception_ptr launch_error;
    try {
      detail::schedule_join_tasks(
          slots, completion, std::index_sequence_for<Ts...>{}, std::move(children)...);
    } catch (...) {
      launch_error = std::current_exception();
    }
    completion.finish_scheduling();
    co_await completion.wait(std::move(drain_target));
    if (launch_error)
      std::rethrow_exception(launch_error);
    co_return detail::collect_join(slots, std::index_sequence_for<Ts...>{});
  }
}

}  // namespace detail

/** @brief 所有子任务使用同一具体调度器类型；空输入默认使用运行时调度器。 */
template <class S = default_scheduler, class... Ts>
task<std::tuple<detail::joined_value<Ts>...>, S> join(task<Ts, S>... children) {
  return detail::join_impl<S>(std::move(children)...);
}

/**
 * @brief 惰性组合动态数量的同类型任务，返回与输入数量和顺序对应的 vector。
 * 空集合执行后返回空 vector。
 */
template <class T, class S = default_scheduler>
task<std::vector<detail::joined_value<T>>, S> join_all(std::vector<task<T, S>> children) {
  std::vector<detail::joined_value<T>> result;
  if (children.empty())
    co_return result;
  auto drain_target = ::faio::detail::capture_current_resume_target<S>();
  std::vector<detail::join_slot<T>> slots(children.size());
  detail::join_completion<S> completion;
  std::exception_ptr launch_error;
  try {
    detail::schedule_join_tasks(children, std::span{slots}, completion);
  } catch (...) {
    launch_error = std::current_exception();
  }
  completion.finish_scheduling();
  co_await completion.wait(std::move(drain_target));
  if (launch_error)
    std::rethrow_exception(launch_error);
  result.reserve(slots.size());
  for (auto& slot : slots)
    result.push_back(slot.take());
  co_return result;
}

}  // namespace faio
