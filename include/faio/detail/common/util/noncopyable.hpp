/**
 * @file faio/detail/common/util/noncopyable.hpp
 * @brief 禁止复制的基类。
 */
#ifndef FAIO_DETAIL_COMMON_UTIL_NONCOPYABLE_HPP
#define FAIO_DETAIL_COMMON_UTIL_NONCOPYABLE_HPP

namespace faio::util {
/** @brief 禁止复制的基类。 */
class noncopyable {
 public:
  /** @brief 禁止复制构造。 */
  noncopyable(const noncopyable&) = delete;

  /** @brief 禁止复制赋值。 */
  noncopyable& operator=(const noncopyable&) = delete;

 protected:
  /** @brief 构造基类对象。 */
  noncopyable() = default;

  /** @brief 销毁基类对象。 */
  ~noncopyable() noexcept = default;
};
}  // namespace faio::util
#endif
