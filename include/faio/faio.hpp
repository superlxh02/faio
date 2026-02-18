#ifndef FAIO_FAIO_HPP
#define FAIO_FAIO_HPP
#include "faio/detail/io.hpp"
#include "faio/detail/net.hpp"
#include "faio/detail/runtime/context.hpp"
#include "faio/detail/sync.hpp"
#include "faio/detail/task.hpp"
#include "faio/detail/time.hpp"

namespace faio {

using runtime_context = runtime::detail::runtime_context;

// spawn: 轻量提交协程
template <typename T> inline void spawn(task<T> &&t) {
  runtime_context::spawn(std::move(t));
}

// block_on: 阻塞执行协程
template <typename T>
inline auto block_on(runtime_context &ctx, task<T> t) -> T {
  return ctx.block_on(std::move(t));
}

// wait_all: 并行执行多个协程
template <typename... Ts>
inline auto wait_all(runtime_context &ctx, task<Ts>... tasks)
    -> std::tuple<Ts...> {
  return ctx.wait_all(std::move(tasks)...);
}

class ConfigBuilder {
public:
  ConfigBuilder() = default;
  ~ConfigBuilder() = default;

public:
  ConfigBuilder &set_num_events(std::size_t num_events) {
    _config._num_events = num_events;
    return *this;
  }

  ConfigBuilder &set_submit_interval(uint32_t submit_interval) {
    _config._submit_interval = submit_interval;
    return *this;
  }

  ConfigBuilder &set_num_workers(std::size_t num_workers) {
    _config._num_workers = num_workers;
    return *this;
  }

  ConfigBuilder &set_io_interval(uint32_t io_interval) {
    _config._io_interval = io_interval;
    return *this;
  }

  ConfigBuilder &set_global_queue_interval(uint32_t global_queue_interval) {
    _config._global_queue_interval = global_queue_interval;
    return *this;
  }

  runtime::detail::Config build() { return _config; }

private:
  runtime::detail::Config _config;
};
} // namespace faio

#endif // FAIO_FAIO_HPP
