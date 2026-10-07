#include <cassert>
#include <faio/experimental/async_main.h>

int application_entry(double) { return 13; }
int application_entry(const char*) { return 19; }

[[=faio::experimental::main(application_entry)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread,
    .workers = 1,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> application_entry(int argc, char** argv) {
  assert(argc > 0 && argv != nullptr && argv[argc] == nullptr);
  co_return 41;
}

static_assert(std::meta::identifier_of(
    faio::experimental::detail::entry_function_of(application_entry)) == "application_entry");
#ifdef main
#error "Selecting a named overload must restore main"
#endif
