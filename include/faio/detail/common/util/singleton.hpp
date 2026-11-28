/**
 * @file faio/detail/common/util/singleton.hpp
 * @brief 单例访问工具。
 */
#ifndef FAIO_DETAIL_COMMON_UTIL_SINGLETON_HPP
#define FAIO_DETAIL_COMMON_UTIL_SINGLETON_HPP

namespace faio::util {
/** @brief 单例对象访问器。 */
template <typename T>
class singleton {
  /** @brief 禁止创建单例访问器实例。 */
  singleton() = delete;

  /** @brief 禁止销毁单例访问器实例。 */
  ~singleton() = delete;

 public:
  /** @brief 取得单例对象。 */
  [[nodiscard]]
  static auto instance() -> T& {
    static T instance;
    return instance;
  }
};
}  // namespace faio::util
#endif
