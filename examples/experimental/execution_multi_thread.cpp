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

exec::task<int> external_job(faio::experimental::multi_thread_runtime& runtime) {
    co_await stdexec::schedule(runtime.get_scheduler());
    co_return co_await runtime.as_sender(native_job());
}

int main() {
    faio::experimental::multi_thread_runtime runtime{
        faio::experimental::runtime_options{
            .mode = faio::runtime::mode::multi_thread,
            .workers = 4,
            .io_backend = faio::runtime::io_backend::IO_EPOLL
        }};
    const auto result = runtime.run(external_job(runtime));
    return result && std::get<0>(*result) == 42 ? 0 : 1;
}
