#define ASYNC(...) existing_macro
#include <cassert>
#include <type_traits>
#include <faio/experimental/async_main.h>
#include <faio/experimental/async_main.h>

#ifndef ASYNC
#error "The entry header must preserve unrelated macros"
#endif
#ifndef main
#error "The entry header must arm its one-shot main macro"
#endif

[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::multi_thread,
    .workers = 2,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> async_main(int argc, char** argv) {
  assert(argc > 0 && argv != nullptr && argv[argc] == nullptr);
  co_return 0;
}

static_assert(faio::experimental::runtime_options_of<^^async_main>().workers == 2);
static_assert(std::meta::annotations_of_with_type(^^async_main,
    ^^faio::experimental::main_annotation_type).size() == 1);

#ifdef main
#error "The entry marker must restore the main token after expansion"
#endif
#include <faio/experimental/async_main.h>
#ifdef main
#error "Including the guarded entry header again must not rearm its macro"
#endif
struct ordinary_identifier {
  static constexpr int main = 42;
};
static_assert(ordinary_identifier::main == 42);
static_assert(std::is_same_v<decltype(faio::experimental::main),
                             const faio::experimental::main_annotation_type>);
