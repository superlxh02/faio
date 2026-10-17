/**
 * @file faio/detail/common/util/rand.hpp
 * @brief 公共类型、错误与基础工具：线程随机数。
 * @details 供 faio 各模块复用的头文件接口；具体所有权、错误与线程访问约束见类型及成员说明。
 */
#ifndef FAIO_DETAIL_COMMON_UTIL_RAND_HPP
#define FAIO_DETAIL_COMMON_UTIL_RAND_HPP

#include <random>

namespace faio::util {
class FastRand {
 public:
  FastRand() {
    std::random_device dev;
    one_ = static_cast<uint32_t>(dev());
    two_ = static_cast<uint32_t>(dev());
  }

  auto fastrand_n(uint32_t n) -> uint32_t {
    auto expected = static_cast<uint64_t>(fastrand()) * static_cast<uint64_t>(n);
    return static_cast<uint32_t>(expected >> 32);
  }

 private:
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
  uint32_t one_;  ///< 快速伪随机数生成器的第一组内部状态。
  uint32_t two_;  ///< 快速伪随机数生成器的第二组内部状态。
};
}
#endif
