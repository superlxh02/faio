/**
 * @file faio/detail/experimental/async_entry_macro.hpp
 * @brief 实验性的入口注解与发送器接收器适配：入口函数宏展开。
 * @details 实验适配在编译期开关下启用，连接阶段建立稳定操作状态；启动、结果交付与关闭沿受控任务图的租约和排空协议推进。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_ASYNC_ENTRY_MACRO_HPP
#define FAIO_DETAIL_EXPERIMENTAL_ASYNC_ENTRY_MACRO_HPP

/// @brief 仅入口翻译单元为预处理标记 main 保留函数式宏；限定名称仍会触发展开。首次展开恢复其未定义状态，底层 constexpr 注解对象继续可用。
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
