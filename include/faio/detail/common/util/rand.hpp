/**
 * @file faio/detail/common/util/rand.hpp
 * @brief 线程局部伪随机数生成器。
 */
#ifndef FAIO_DETAIL_COMMON_UTIL_RAND_HPP
#define FAIO_DETAIL_COMMON_UTIL_RAND_HPP

#include <random>

namespace faio::util {
/** @brief 线程局部快速伪随机数生成器。 */
class fast_rand {
 public:
  /** @brief 初始化线程局部伪随机数生成器。 */
  fast_rand() {
    std::random_device dev;
    one_ = static_cast<uint32_t>(dev());
    two_ = static_cast<uint32_t>(dev());
  }

  /** @brief 生成指定范围内的伪随机数。 */
  auto fastrand_n(uint32_t n) -> uint32_t {
    auto expected = static_cast<uint64_t>(fastrand()) * static_cast<uint64_t>(n);
    return static_cast<uint32_t>(expected >> 32);
  }

 private:
  /** @brief 生成快速伪随机数。 */
  auto fastrand() -> uint32_t {
    auto s1 = one_;
    auto s0 = two_;
    s1 ^= s1 << 17;
    s1 = s1 ^ s0 ^ s1 >> 7 ^ s0 >> 16;
    one_ = s0;
    two_ = s1;
    return s0 + s1;
  }

 private:
  uint32_t one_; /**< 生成器的第一组内部状态。 */
  uint32_t two_; /**< 生成器的第二组内部状态。 */
};
}  // namespace faio::util
#endif
