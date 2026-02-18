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

} // namespace faio::util
#endif // FAIO_DETAIL_COMMON_STATIC_MATH_HPP