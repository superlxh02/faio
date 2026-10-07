#include <cassert>
#include <faio/experimental/async_main.h>

[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread,
    .workers = 1,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
[[=faio::experimental::main(run_application)]]
faio::task<int> run_application(int argc, char** argv) {
  assert(argc > 0 && argv != nullptr && argv[argc] == nullptr);
  co_return 37;
}

#ifdef main
#error "The one-shot entry macro must be restored with either annotation order"
#endif

static_assert(faio::experimental::detail::entry_options<^^run_application>().workers == 1);
static_assert(faio::experimental::detail::entry_options<^^run_application>().mode ==
              faio::runtime::mode::current_thread);
