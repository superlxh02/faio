#include <faio/experimental/runtime.h>
#ifdef ASYNC
#error "runtime.h must not define ASYNC"
#endif
int main() {
    faio::experimental::current_thread_runtime runtime{
        faio::experimental::runtime_options{
            .mode = faio::runtime::mode::current_thread,
            .workers = 1,
            .io_backend = faio::runtime::io_backend::IO_EPOLL
        }};
    runtime.shutdown();
    return 0;
}
