#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <exec/task.hpp>
#include <chrono>
#include <tuple>
#include <utility>
#include <memory>
#include <stdexcept>
#include <faio/experimental/async_main.h>

faio::task<int> timer_value(const void* expected_context) {
  if (std::addressof(faio::experimental::this_runtime().context()) != expected_context)
    throw std::logic_error("native task used a different runtime");
  co_await faio::time::sleep(std::chrono::milliseconds{1});
  co_return 42;
}

exec::task<int> workflow(faio::experimental::runtime_ref runtime) {
  // Capture the runtime explicitly: exec::task does not propagate faio queries.
  co_await stdexec::schedule(runtime.get_multi_thread_scheduler());
  const auto expected_context = std::addressof(runtime.context());
  if (std::addressof(faio::experimental::this_runtime().context()) != expected_context)
    throw std::logic_error("external task used a different runtime");
  const auto value = co_await runtime.as_sender(timer_value(expected_context));
  co_return value;
}

[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::multi_thread,
    .workers = 4,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> async_main(int, char**) {
  auto runtime = faio::experimental::this_runtime();
  auto result = runtime.spawn_sender(workflow(runtime));
  const auto values = co_await std::move(result);
  co_return values && std::get<0>(*values) == 42 ? 0 : 1;
}
