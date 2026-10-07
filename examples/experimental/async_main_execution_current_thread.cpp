#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <exec/task.hpp>
#include <chrono>
#include <tuple>
#include <utility>
#include <faio/experimental/async_main.h>

faio::task<int> native_job(const void* expected_context) {
    if (&faio::experimental::this_runtime().context() != expected_context)
        co_return -1;
    co_await faio::time::sleep(std::chrono::milliseconds{1});
    co_return 42;
}

exec::task<int> external_job(faio::experimental::runtime_ref runtime) {
    co_await stdexec::schedule(runtime.get_current_thread_scheduler());
    if (&faio::experimental::this_runtime().context() != &runtime.context())
        co_return -1;
    co_return co_await runtime.as_sender(native_job(&runtime.context()));
}

[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread,
    .workers = 1,
    .io_backend = faio::runtime::io_backend::IO_EPOLL
}]]
faio::task<int> async_main(int, char**) {
    auto runtime = faio::experimental::this_runtime();
    auto joined = runtime.spawn_sender(external_job(runtime));
    const auto result = co_await std::move(joined);
    co_return result && std::get<0>(*result) == 42 ? 0 : 1;
}
