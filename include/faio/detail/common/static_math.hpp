/**
 * @file faio/detail/common/static_math.hpp
 * @brief 编译期整数运算。
 */
#ifndef FAIO_DETAIL_COMMON_STATIC_MATH_HPP
#define FAIO_DETAIL_COMMON_STATIC_MATH_HPP

#include <type_traits>

namespace faio::util {
/** @brief 在编译期计算整数幂。 */
template <typename T>
  requires std::is_integral_v<T>
static inline consteval auto static_pow(T x, T y) -> T {
  T expected = 1;
  while (y--) {
    expected *= x;
  }
  return expected;
}

/** @brief 在编译期计算整数对数。 */
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
}  // namespace faio::util
#endif
