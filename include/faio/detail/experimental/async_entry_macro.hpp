/**
 * @file faio/detail/experimental/async_entry_macro.hpp
 * @brief 入口函数宏展开。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_ASYNC_ENTRY_MACRO_HPP
#define FAIO_DETAIL_EXPERIMENTAL_ASYNC_ENTRY_MACRO_HPP

#ifdef main
#error "faio experimental: async_main.h requires main not to be a macro"
#else
#pragma push_macro("main")
#define main(entry_name)                                                                       \
  _Pragma("pop_macro(\"main\")") main]]                                         \
  faio::task<int> entry_name(int, char**);                                                     \
  int main(int faio_argc, char** faio_argv) {                                                  \
    return faio::experimental::detail::run_main<faio::experimental::detail::entry_function_of( \
        ::entry_name)>(faio_argc, faio_argv);                                                  \
  }                                                                                            \
  [[maybe_unused
#endif

#endif
