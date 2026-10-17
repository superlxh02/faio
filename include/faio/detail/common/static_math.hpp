/**
 * @file faio/detail/common/static_math.hpp
 * @brief 公共类型、错误与基础工具：编译期整数运算。
 * @details 供 faio 各模块复用的头文件接口；具体所有权、错误与线程访问约束见类型及成员说明。
 */
#ifndef FAIO_DETAIL_COMMON_STATIC_MATH_HPP
#define FAIO_DETAIL_COMMON_STATIC_MATH_HPP

#include <concepts>

namespace faio::util {
template <typename T>
  requires std::is_integral_v<T>
static inline consteval auto static_pow(T x, T y) -> T {
  T expected = 1;
  while (y--) {
    expected *= x;
  }
  return expected;
}

template <typename T>
  requires std::is_integral_v<T>
static inline consteval auto static_log(T x, T y) -> T {
  T expected = 0;
  while (x != 1) {
    x /= y;
    expected += 1;
  }
  return expected;
}
}
#endif
