#ifndef FAIO_DETAIL_COMMON_UTIL_SINGLETON_HPP
#define FAIO_DETAIL_COMMON_UTIL_SINGLETON_HPP
namespace faio::util {

template <typename T> class Singleton {
  Singleton() = delete;
  ~Singleton() = delete;

public:
  [[nodiscard]]
  static auto instance() -> T & {
    static T instance;
    return instance;
  }
};

} // namespace faio::util
#endif // FAIO_DETAIL_COMMON_UTIL_SINGLETON_HPP