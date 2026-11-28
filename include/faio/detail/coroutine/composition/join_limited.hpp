/**
 * @file faio/detail/coroutine/composition/join_limited.hpp
 * @brief 以固定窗口限制同时接纳的任务数并按输入顺序收集终态。
 */
#pragma once

#include "faio/detail/coroutine/composition/task_group.hpp"
#include <algorithm>
#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace faio {
namespace detail {
/**
 * @brief 词法任务组的主体按输入顺序接纳任务，回收最早接纳的任务后再补充窗口。
 * @details 窗口和结果空间在首次接纳前准备。尝试接纳失败后先释放未开始的帧，再等待已经接纳的任务。
 */
template <class T, class S>
task<std::vector<task_outcome<T>>, S> join_limited_body(basic_task_group_context<S>& context,
                                                        std::size_t limit,
                                                        std::vector<task<T, S>> children) {
  const auto size = children.size();
  const auto width = std::min(limit, size);
  std::vector<std::optional<scoped_join_handle<T, S>>> active(width);
  std::vector<task_outcome<T>> results;
  results.reserve(size);

  std::size_t next{};
  std::size_t received{};
  bool accepting = true;
  std::exception_ptr admission_error;
  while (accepting || received < next) {
    if (received == next && context.stop_token().stop_requested()) {
      children.clear();
      break;
    }
    basic_resume_target<S> wait_target;
    try {
      wait_target = capture_current_resume_target<S>();
    } catch (...) {
      context.request_stop();
      throw;
    }
    while (accepting && next < size && next - received < width) {
      try {
        auto handle = context.try_spawn_observed(children[next]);
        if (handle) {
          active[next % width].emplace(std::move(*handle));
          ++next;
          continue;
        }
      } catch (...) {
        admission_error = std::current_exception();
        context.request_stop();
      }
      children.clear();
      accepting = false;
    }
    if (next == size)
      accepting = false;
    if (received < next) {
      auto& handle = active[received % width];
      results.emplace_back(co_await handle->wait(std::move(wait_target)));
      handle.reset();
      ++received;
    }
  }

  if (admission_error)
    results.emplace_back(task_outcome<T>::from_error(std::move(admission_error)));
  while (results.size() < size)
    results.emplace_back(task_outcome<T>::from_stopped(task_stop_reason::NOT_STARTED));
  co_return results;
}

/** @brief 等待词法组排空，再交付按输入位置排列的终态。 */
template <class T, class S>
task<std::vector<task_outcome<T>>, S> join_limited_group(std::size_t limit,
                                                         std::vector<task<T, S>> children,
                                                         group_options options) {
  if (children.empty())
    co_return std::vector<task_outcome<T>>{};
  auto result = co_await task_group<S>(
      options,
      [limit, children = std::move(children)](basic_task_group_context<S>& context) mutable {
        return join_limited_body(context, limit, std::move(children));
      });
  co_return result.body.value_or_throw();
}
}  // namespace detail

/**
 * @brief 惰性运行同类型任务，按输入顺序返回每项终态。
 * @details 同时接纳的任务最多为 limit 个。回收窗口中最早接纳的任务后补充窗口。
 * 父级停止或 fail_fast 触发后，未接纳的任务释放并记为 not_started，已接纳的任务仍排空。
 * 投递失败记录在对应输入项中；若无法预留组合器自身的恢复槽，组合任务会请求停止，排空后再抛出异常。
 * @param limit 同时接纳的任务数上限，必须为正数。
 * @param children 交给词法任务组的惰性任务。
 * @param options 处理已接纳任务失败的组策略，默认 fail_fast。
 * @return 惰性结果任务，在任务组排空后交付结果。
 * @throws std::invalid_argument limit 为零。
 */
template <class T, class S>
task<std::vector<task_outcome<T>>, S> join_all_limited(std::size_t limit,
                                                       std::vector<task<T, S>> children,
                                                       group_options options = {}) {
  if (limit == 0)
    throw std::invalid_argument("join_all_limited 的并发上限必须为正数");
  return detail::join_limited_group(limit, std::move(children), options);
}
}  // namespace faio
