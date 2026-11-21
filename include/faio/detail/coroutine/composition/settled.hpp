/**
 * @file faio/detail/coroutine/composition/settled.hpp
 * @brief 收集全部任务的显式终态，并按输入顺序交付。
 */
#pragma once

#include "faio/detail/coroutine/composition/task_group.hpp"
#include <cstddef>
#include <tuple>
#include <utility>
#include <vector>

namespace faio {
namespace detail {
/** @brief 先接纳全部分支，再按输入顺序消费已独立执行的结果。 */
template <class S, class... T, std::size_t... I>
task<std::tuple<task_outcome<T>...>, S> collect_settled(basic_task_group_context<S>& context,
                                                        std::tuple<task<T, S>...> input,
                                                        std::index_sequence<I...>) {
  auto handles = std::tuple<scoped_join_handle<T, S>...>{
      context.spawn_observed(std::move(std::get<I>(input)))...};
  co_return std::tuple<task_outcome<T>...>{(co_await std::get<I>(handles))...};
}

/** @brief 在启动任何子任务前准备动态汇总容器和等待者空间。 */
template <class T, class S>
task<std::vector<task_outcome<T>>, S> collect_settled(basic_task_group_context<S>& context,
                                                      std::vector<task<T, S>> input) {
  std::vector<scoped_join_handle<T, S>> handles;
  std::vector<task_outcome<T>> results;
  handles.reserve(input.size());
  results.reserve(input.size());
  for (auto& task : input)
    handles.push_back(context.spawn_observed(std::move(task)));
  for (auto& handle : handles)
    results.push_back(co_await handle);
  co_return results;
}
}  // namespace detail

/**
 * @brief 并发执行异构任务，在全部退出后按参数顺序交付终态。
 * @details 工作异常与停止保存在结果槽，不触发同组任务停止。接纳或恢复容量等基础设施失败
 * 由组合任务传播，交付前仍排空已接纳项。
 */
template <class S = default_scheduler, class... T>
task<std::tuple<task_outcome<T>...>, S> join_settled(task<T, S>... children) {
  if constexpr (sizeof...(T) == 0) {
    co_return std::tuple<>{};
  } else {
    auto body = [input = std::tuple{std::move(children)...}](
                    basic_task_group_context<S>& context) mutable {
      return detail::collect_settled(context, std::move(input), std::index_sequence_for<T...>{});
    };
    auto group =
        co_await task_group<S>(group_options{group_error_policy::collect_all}, std::move(body));
    co_return group.body.value_or_throw();
  }
}

/** @brief 同类型动态汇合，保留每个输入的值、异常或停止及其顺序。 */
template <class T, class S>
task<std::vector<task_outcome<T>>, S> join_all_settled(std::vector<task<T, S>> children) {
  if (children.empty())
    co_return std::vector<task_outcome<T>>{};
  auto body = [input = std::move(children)](basic_task_group_context<S>& context) mutable {
    return detail::collect_settled(context, std::move(input));
  };
  auto group =
      co_await task_group<S>(group_options{group_error_policy::collect_all}, std::move(body));
  co_return group.body.value_or_throw();
}
}  // namespace faio
