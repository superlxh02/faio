/**
 * @file faio/detail/coroutine/composition/for_each_concurrent.hpp
 * @brief 在词法异步流上以固定窗口处理元素。
 */
#pragma once
#include "faio/detail/coroutine/stream/async_generator.hpp"
#include <limits>

namespace faio::experimental {
namespace stream_detail {
/** @brief 只保存第一个处理异常，避免为整个数据流积累结果。 */
struct processing_error {
  std::mutex mutex;
  std::exception_ptr first;

  void record(std::exception_ptr error) {
    std::lock_guard lock(mutex);
    if (!first)
      first = std::move(error);
  }
};

template <class Factory, class T, class S>
::faio::detail::root_task stream_item_root_task_wrapper(
    Factory& operation,
    std::optional<T> value,
    typename sync::basic_semaphore<S>::permit permit,
    ::faio::detail::basic_task_group_state<S>* owner,
    processing_error* failures) {
  {
    auto active_slot = std::move(permit);
    try {
      co_await std::invoke(operation, std::move(*value));
    } catch (const operation_cancelled&) {
      if (!owner->stop_owner->source.stop_requested()) {
        failures->record(std::current_exception());
        owner->request_stop();
      }
    } catch (...) {
      failures->record(std::current_exception());
      owner->request_stop();
    }
    value.reset();
  }
  owner->finish();
}
}  // namespace stream_detail

/**
 * @brief 进行中的拉取与已接纳的处理合计最多为 limit 个。
 * @details 借用与 with_stream 主体同寿命的读端。首次处理出错时，停止其他处理和待完成的 next，
 * 当前函数退出前排空已接纳的操作，外层 with_stream 随后停止并排空生产者。
 * operation 工厂由调用方按值传入，多个处理可以并行调用它。
 */
template <class T, class S, class Factory>
  requires std::same_as<std::invoke_result_t<Factory&, T>, task<void, S>>
task<void, S> for_each_concurrent(std::size_t limit,
                                  stream_reader<T, S>& input,
                                  Factory operation) {
  if (limit == 0 || limit > std::numeric_limits<std::ptrdiff_t>::max())
    throw std::invalid_argument("处理窗口大小无效");
  sync::basic_semaphore<S> permits{static_cast<std::ptrdiff_t>(limit)};
  stream_detail::processing_error failures;
  auto result = co_await task_group<S>([&](basic_task_group_context<S>& group) -> task<void, S> {
    auto& owner = ::faio::detail::group_lifetime_access::state(group);
    try {
      for (;;) {
        if (group.stop_token().stop_requested())
          throw operation_cancelled{};
        auto permit = co_await permits.acquire_permit();
        if (group.stop_token().stop_requested())
          throw operation_cancelled{};
        auto value = co_await input.next();
        if (!value)
          co_return;
        auto root = stream_detail::stream_item_root_task_wrapper<Factory, T, S>(
            operation, std::move(value), std::move(permit), &owner, &failures);
        ::faio::detail::group_lifetime_access::schedule_task(
            group, std::move(root), owner.stop_owner);
      }
    } catch (const operation_cancelled&) {
      if (!group.stop_token().stop_requested())
        failures.record(std::current_exception());
      throw;
    } catch (...) {
      failures.record(std::current_exception());
      throw;
    }
  });
  if (failures.first)
    std::rethrow_exception(failures.first);
  result.body.value_or_throw();
}

/**
 * @brief 持有流工厂，启动后拉取并处理元素，并在本次调用结束前完成关停。
 */
template <class T, class S = default_scheduler, class StreamFactory, class OperationFactory>
task<void, S> for_each_concurrent(std::size_t limit,
                                  StreamFactory factory,
                                  OperationFactory operation) {
  co_await with_stream<T, S>(
      std::move(factory),
      [limit, operation = std::move(operation)](stream_reader<T, S>& input) mutable {
        return for_each_concurrent(limit, input, std::move(operation));
      });
}
}  // namespace faio::experimental

namespace faio {
using experimental::for_each_concurrent;
}  // namespace faio
