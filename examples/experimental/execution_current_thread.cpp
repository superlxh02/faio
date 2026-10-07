#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>
#include <chrono>
#include <tuple>

faio::task<int> native_job() {
    co_await faio::time::sleep(std::chrono::milliseconds{1});
    co_return 42;
}

exec::task<int> external_job(faio::experimental::runtime_ref runtime) {
    co_await stdexec::schedule(runtime.get_current_thread_scheduler());
    co_return co_await runtime.as_sender(native_job());
}

int main() {
    faio::experimental::current_thread_runtime runtime{
        faio::experimental::runtime_options{
            .mode = faio::runtime::mode::current_thread,
            .workers = 1,
            .io_backend = faio::runtime::io_backend::IO_EPOLL
        }};
    const auto result = runtime.run(external_job(runtime.ref()));
    return result && std::get<0>(*result) == 42 ? 0 : 1;
}
