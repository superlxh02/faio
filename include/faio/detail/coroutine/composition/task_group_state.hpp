/**
 * @file faio/detail/coroutine/composition/task_group_state.hpp
 * @brief 任务组的接纳、退出状态和独立子结果槽。
 */
#pragma once

#include "faio/detail/coroutine/composition/result.hpp"
#include "faio/detail/coroutine/launch/root_task.hpp"
#include "faio/detail/coroutine/wait/completion_event.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace faio {
/** @brief 子任务失败后是否对同组工作发出停止请求。 */
enum class group_error_policy { fail_fast, collect_all };

/** @brief 任务组策略，默认在首次观察到错误时发出停止请求。 */
struct group_options {
  group_error_policy errors{group_error_policy::fail_fast};
};

/** @brief 任务组接纳错误。 */
enum class task_group_errc { closed = 1 };

/** @brief 取得任务组错误码所属的类别。 */
inline const std::error_category& task_group_error_category() noexcept {
  struct category final : std::error_category {
    const char* name() const noexcept override { return "faio.task_group"; }

    std::string message(int code) const override {
      return code == static_cast<int>(task_group_errc::closed) ? "任务组已关闭接纳"
                                                               : "未知任务组错误";
    }
  };

  static const category value;
  return value;
}

/** @brief 将任务组接纳错误转换为可比较的错误码。 */
inline std::error_code make_error_code(task_group_errc error) noexcept {
  return {static_cast<int>(error), task_group_error_category()};
}

/** @brief 子任务的终态摘要，按最终接纳顺序保存，不持有成功值。 */
struct group_child_result {
  std::size_t index{};
  task_outcome_status status{};
  std::exception_ptr exception;
};

/** @brief 已排空的任务组结果，独立持有主体终态及子任务元数据。 */
template <class T>
struct group_result {
  task_outcome<T> body;
  std::vector<group_child_result> children;
};

namespace detail {
/** @brief 词法句柄独立持有的单次结果槽。组汇总不共享其业务值。 */
template <class T, class S>
struct group_value_slot {
  explicit group_value_slot(std::shared_ptr<std::atomic<bool>> body_open)
      : body_active(std::move(body_open)), ready(std::make_shared<basic_completion_event<S>>()) {}

  std::shared_ptr<std::atomic<bool>> body_active;
  std::optional<task_outcome<T>> outcome;
  std::shared_ptr<basic_completion_event<S>> ready;
};

/** @brief 仅在任务组退出前使用，最后一份工作责任归还后发布排空状态。 */
template <class S>
class basic_task_group_state {
 public:
  basic_task_group_state(scheduler_ref_for<S> scheduler, task_context parent, group_options options)
      : scheduler(scheduler),
        lifetime(current_task_lifetime()),
        parent(std::move(parent)),
        stop_owner(std::make_shared<linked_stop_state>(this->parent.stop_token,
                                                       this->parent.cancellation_owner)),
        body_active(std::make_shared<std::atomic<bool>>(true)),
        options_(options) {}

  /** @brief 短暂持锁以登记工作；可选的停止检查仅用于限制并发时补充接纳任务。 */
  std::shared_ptr<group_child_result> admit(bool stop_sensitive) {
    std::lock_guard lock(mutex_);
    if (phase_ != phase::open)
      throw std::system_error(make_error_code(task_group_errc::closed));
    if (stop_sensitive && (stopping_ || stop_owner->source.stop_requested()))
      return {};
    auto record = std::make_shared<group_child_result>();
    records_.push_back(record);
    ++pending_;
    return record;
  }

  /** @brief 登记由组合自身观察终态的工作，不加入组结果列表。 */
  void admit_lifetime() {
    std::lock_guard lock(mutex_);
    if (phase_ != phase::open)
      throw std::system_error(make_error_code(task_group_errc::closed));
    ++pending_;
  }

  /** @brief 未发布或提交失败时，从接纳序列中删除记录并归还对应计数。 */
  void rollback(const std::shared_ptr<group_child_result>& record) {
    const bool drained = [&] {
      std::lock_guard lock(mutex_);
      const auto found = std::ranges::find(records_, record);
      records_.erase(found);
      return release_locked();
    }();
    if (drained)
      completion.notify();
  }

  /** @brief 由词法范围内的主体关闭接纳，已登记任务继续排空。 */
  void close() {
    const bool drained = [&] {
      std::lock_guard lock(mutex_);
      if (phase_ == phase::open)
        phase_ = phase::closing;
      return mark_drained_locked();
    }();
    if (drained)
      completion.notify();
  }

  /** @brief 在锁外发出请求，允许停止回调重新进入本组。 */
  void request_stop() noexcept { stop_owner->source.request_stop(); }

  /** @brief 记录首个错误后，在锁外广播停止请求。 */
  void observe_error() {
    bool request = false;
    if (options_.errors == group_error_policy::fail_fast) {
      std::lock_guard lock(mutex_);
      request = !std::exchange(stopping_, true);
    }
    if (request)
      request_stop();
  }

  /** @brief 主体退出时撤销接纳及消费权限，主体所占计数保留到退出处理完成。 */
  void close_body() {
    body_active->store(false, std::memory_order_release);
    close();
  }

  /** @brief 结束此工作持有的业务和组访问责任，并归还计数。 */
  void finish() {
    const bool drained = [&] {
      std::lock_guard lock(mutex_);
      return release_locked();
    }();
    if (drained)
      completion.notify();
  }

  /** @brief 排空后按接纳顺序取出元数据，压缩未发布的接纳项。 */
  std::vector<group_child_result> collect() {
    std::vector<group_child_result> results;
    results.reserve(records_.size());
    for (auto& record : records_) {
      record->index = results.size();
      results.push_back(std::move(*record));
    }
    return results;
  }

  const scheduler_ref_for<S> scheduler;
  const task_lifetime_ref lifetime;
  const task_context parent;
  const std::shared_ptr<linked_stop_state> stop_owner;
  const std::shared_ptr<std::atomic<bool>> body_active;
  basic_completion_event<S> completion;

 private:
  enum class phase { open, closing, drained };

  bool release_locked() noexcept {
    --pending_;
    return mark_drained_locked();
  }

  bool mark_drained_locked() noexcept {
    if (phase_ != phase::closing || pending_ != 0)
      return false;
    phase_ = phase::drained;
    return true;
  }

  const group_options options_;
  std::mutex mutex_;
  phase phase_{phase::open};
  std::size_t pending_{1};  ///< 主体占用初始计数，确保其返回前任务组不会被视为已排空。
  bool stopping_{};
  std::vector<std::shared_ptr<group_child_result>> records_;
};
}  // namespace detail
}  // namespace faio
