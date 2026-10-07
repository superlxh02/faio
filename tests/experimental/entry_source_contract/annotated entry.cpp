#include "relative.hpp"
#include <entry_options.hpp>
#include <cassert>
#include <faio/experimental/async_main.h>

#ifndef FAIO_ENTRY_SOURCE_OPTION
#error "The entry must preserve source compile definitions"
#endif
static_assert(FAIO_ENTRY_SOURCE_OPTION == expected_entry_option);

[[=faio::experimental::main(custom_entry)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::current_thread,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> custom_entry(int argc, char** argv) {
  assert(argc > 0 && argv != nullptr);
  assert(entry_helper_value() == expected_entry_option);
  co_return 0;
}
