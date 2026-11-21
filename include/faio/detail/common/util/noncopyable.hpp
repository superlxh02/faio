/**
 * @file faio/detail/common/util/noncopyable.hpp
 * @brief 禁止复制的基类。
 */
#ifndef FAIO_DETAIL_COMMON_UTIL_NONCOPYABLE_HPP
#define FAIO_DETAIL_COMMON_UTIL_NONCOPYABLE_HPP

namespace faio::util {
/** @brief 禁止复制的基类。 */
class Noncopyable {
 public:
  /** @brief 禁止复制构造。 */
  Noncopyable(const Noncopyable&) = delete;

  /** @brief 禁止复制赋值。 */
  Noncopyable& operator=(const Noncopyable&) = delete;

 protected:
  /** @brief 构造基类对象。 */
  Noncopyable() = default;

  /** @brief 销毁基类对象。 */
  ~Noncopyable() noexcept = default;
};
}  // namespace faio::util
#endif
