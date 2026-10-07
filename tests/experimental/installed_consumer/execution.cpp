#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <exec/task.hpp>
#include <chrono>
#include <tuple>
#ifdef ASYNC
#error "execution.h must not define ASYNC"
#endif
faio::task<int> native_work() {
    co_await faio::time::sleep(std::chrono::milliseconds{1});
    co_return 42;
}
exec::task<int> external_work(faio::experimental::runtime_ref runtime) {
    co_return co_await runtime.as_sender(native_work());
}
int main() {
    faio::experimental::current_thread_runtime runtime{
        faio::experimental::runtime_options{
            .mode = faio::runtime::mode::current_thread,
            .workers = 1,
            .io_backend = faio::runtime::io_backend::IO_EPOLL
        }};
    auto value = runtime.run(external_work(runtime.ref()));
    return value && std::get<0>(*value) == 42 ? 0 : 1;
}
