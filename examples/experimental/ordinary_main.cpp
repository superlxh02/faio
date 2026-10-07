#include <faio/experimental/runtime_options.h>
#include <faio/faio.hpp>
#include <chrono>

faio::task<int> application() {
    co_await faio::time::sleep(std::chrono::milliseconds{1});
    co_return 0;
}

int main() {
    faio::runtime::configure(faio::experimental::to_runtime_config(
        faio::experimental::runtime_options{
            .mode = faio::runtime::mode::current_thread,
            .workers = 1,
            .io_backend = faio::runtime::io_backend::IO_EPOLL
        }));
    const auto result = faio::block_on(application());
    faio::runtime::shutdown();
    return result;
}
