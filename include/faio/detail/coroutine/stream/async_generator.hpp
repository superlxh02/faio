/**
 * @file faio/detail/coroutine/stream/async_generator.hpp
 * @brief 容量为一、无预取的词法拉取流。
 */
#pragma once

#include "faio/detail/coroutine/composition/task_group.hpp"
#include "faio/detail/coroutine/composition/with_cleanup.hpp"
#include "faio/detail/coroutine/composition/with_stop_token.hpp"
#include "faio/detail/sync/semaphore.hpp"

namespace faio::experimental {
namespace stream_detail {
/** @brief 每次拉取与产出都通过单个任务的许可交接，close 负责排空生产者和正在进行的 next。 */
template <class T, class S>
struct state {
  sync::basic_semaphore<S> demand;
  sync::basic_semaphore<S> response;
  sync::basic_semaphore<S> next_returned;
  std::mutex mutex;
  std::stop_source stop;
  std::optional<T> value;
  std::exception_ptr failure;
  bool pending{};
  bool closing{};
  bool read_eof{};
  bool failure_delivered{};
};

template <class T, class S>
class producer {
 public:
  explicit producer(std::shared_ptr<state<T, S>> stream) : stream_(std::move(stream)) {}

  /** @brief 交付一个值，并等到下一次拉取许可才让生产者继续。 */
  task<void, S> emit(T value) {
    if (stream_->stop.stop_requested())
      throw operation_cancelled{};
    stream_->value.emplace(std::move(value));
    stream_->response.release();
    co_await stream_->demand.acquire();
    if (stream_->stop.stop_requested())
      throw operation_cancelled{};
  }

 private:
  std::shared_ptr<state<T, S>> stream_;
};

template <class T, class S, class Factory>
task<void, S> produce(std::shared_ptr<state<T, S>> stream, Factory factory) {
  try {
    co_await stream->demand.acquire();
    if (stream->stop.stop_requested())
      throw operation_cancelled{};
    producer<T, S> output{stream};
    co_await std::invoke(factory, output);
  } catch (...) {
    stream->failure = std::current_exception();
  }
  stream->response.release();
}
}  // namespace stream_detail

/** @brief 工厂参数，顺序调用 emit，每一项由对应 next 驱动。 */
template <class T, class S = default_scheduler>
using stream_producer = stream_detail::producer<T, S>;

/** @brief 只能在 with_stream 词法主体中使用的单消费者。 */
template <class T, class S = default_scheduler>
class stream_reader {
 public:
  explicit stream_reader(std::shared_ptr<stream_detail::state<T, S>> state,
                         scheduler_ref_for<S> scheduler)
      : state_(std::move(state)), scheduler_(scheduler) {}

  stream_reader(const stream_reader&) = delete;
  stream_reader& operator=(const stream_reader&) = delete;

  /** @brief 无预取地拉取一项；EOF 返回 nullopt，生产异常原样传播。 */
  task<std::optional<T>, S> next() { return pull(state_, scheduler_); }

 private:
  template <class U, class Scheduler, class Factory, class Body>
  friend auto with_stream(Factory, Body)
      -> std::invoke_result_t<Body&, stream_reader<U, Scheduler>&>;

  /** @brief 词法清理时请求停止，并等待进行中的 next 结束；外层任务组负责排空生产者协程帧。 */
  task<void, S> close() {
    if (!::faio::detail::current_scheduler_matches(scheduler_))
      throw std::logic_error("流的关闭必须在所属调度器内进行");
    const bool pending = [&] {
      std::lock_guard lock(state_->mutex);
      if (state_->closing)
        throw std::logic_error("流的关闭只允许执行一次");
      state_->closing = true;
      return state_->pending;
    }();
    state_->stop.request_stop();
    if (pending)
      co_await state_->next_returned.acquire_uncancellable();
  }

  struct pull_guard {
    stream_detail::state<T, S>& state;

    ~pull_guard() {
      const auto closing = [&] {
        std::lock_guard lock(state.mutex);
        state.pending = false;
        return state.closing;
      }();
      if (closing)
        state.next_returned.release();
    }
  };

  static task<std::optional<T>, S> pull(std::shared_ptr<stream_detail::state<T, S>> state,
                                        scheduler_ref_for<S> scheduler) {
    if (!::faio::detail::current_scheduler_matches(scheduler))
      throw std::logic_error("流的拉取必须在所属调度器内进行");
    {
      std::lock_guard lock(state->mutex);
      if (state->pending || state->closing)
        throw std::logic_error("已有拉取在进行或流已关闭");
      state->pending = true;
    }
    pull_guard guard{*state};
    const auto token = co_await this_coro::stop_token();
    const std::stop_callback stop_waiter{token, [&] { state->stop.request_stop(); }};
    if (token.stop_requested())
      throw operation_cancelled{};
    if (state->read_eof) {
      if (state->failure)
        std::rethrow_exception(state->failure);
      co_return std::nullopt;
    }
    state->demand.release();
    co_await state->response.acquire_uncancellable();
    if (state->value) {
      try {
        auto value = std::move(state->value);
        state->value.reset();
        co_return value;
      } catch (...) {
        state->value.reset();
        state->stop.request_stop();
        throw;
      }
    }
    state->read_eof = true;
    if (state->failure) {
      state->failure_delivered = true;
      std::rethrow_exception(state->failure);
    }
    co_return std::nullopt;
  }

  std::shared_ptr<stream_detail::state<T, S>> state_;
  scheduler_ref_for<S> scheduler_;
};

/**
 * @brief 控制无预取的生产者和单消费者的生命周期。
 * @details 生产者支持显式 emit，按值创建惰性工厂，离开主体时协作停止并排空。
 */
template <class T, class S = default_scheduler, class Factory, class Body>
auto with_stream(Factory factory, Body body) -> std::invoke_result_t<Body&, stream_reader<T, S>&> {
  using Work = std::invoke_result_t<Body&, stream_reader<T, S>&>;
  using R = typename Work::value_type;
  auto result = co_await task_group<S>([&](basic_task_group_context<S>& group) -> Work {
    auto stream = std::make_shared<stream_detail::state<T, S>>();
    stream_reader<T, S> input{stream, co_await this_coro::scheduler()};
    auto producer = stream_detail::produce<T, S>(stream, std::move(factory));
    auto handle =
        group.spawn_observed(with_stop_token(stream->stop.get_token(), std::move(producer)));
    auto operation = with_cleanup([&] { return std::invoke(body, input); },
                                  [&](const task_exit&) -> task<void, S> {
                                    co_await input.close();
                                    (co_await handle).value_or_throw();
                                    if (stream->failure && !stream->failure_delivered) {
                                      try {
                                        std::rethrow_exception(stream->failure);
                                      } catch (const operation_cancelled&) {
                                        if (!stream->stop.stop_requested())
                                          throw;
                                      }
                                    }
                                  });
    if constexpr (std::is_void_v<R>)
      co_await std::move(operation);
    else
      co_return co_await std::move(operation);
  });
  if constexpr (std::is_void_v<R>)
    result.body.value_or_throw();
  else
    co_return result.body.value_or_throw();
}
}  // namespace faio::experimental

namespace faio {
using experimental::stream_producer;
using experimental::stream_reader;
using experimental::with_stream;
}  // namespace faio
