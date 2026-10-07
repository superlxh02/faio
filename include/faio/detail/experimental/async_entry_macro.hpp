#ifndef FAIO_DETAIL_EXPERIMENTAL_ASYNC_ENTRY_MACRO_HPP
#define FAIO_DETAIL_EXPERIMENTAL_ASYNC_ENTRY_MACRO_HPP

// Only the entry TU opts into reserving calls to the preprocessing token `main`.
// Qualification does not prevent expansion. The first call restores it to
// undefined, while the underlying constexpr annotation object stays available.
#ifdef main
#error "faio experimental: async_main.h requires main not to be a macro"
#else
#include "faio/detail/experimental/async_entry.hpp"

#pragma push_macro("main")
#define main(entry_name)                                                       \
  _Pragma("pop_macro(\"main\")") main]]                                         \
  faio::task<int> entry_name(int, char**);                                       \
  int main(int faio_argc, char** faio_argv) {                                    \
    return faio::experimental::detail::run_main<                                \
        faio::experimental::detail::entry_function_of(::entry_name)>(           \
        faio_argc, faio_argv);                                                  \
  }                                                                            \
  [[maybe_unused
#endif

#endif
