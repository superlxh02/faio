#ifndef FAIO_DETAIL_COMMON_UTIL_NONCOPYABLE_HPP
#define FAIO_DETAIL_COMMON_UTIL_NONCOPYABLE_HPP

namespace faio::util {
class Noncopyable {
public:
  Noncopyable(const Noncopyable &) = delete;
  Noncopyable &operator=(const Noncopyable &) = delete;

protected:
  Noncopyable() = default;
  ~Noncopyable() noexcept = default;
};

} // namespace faio::util
#endif // FAIO_DETAIL_COMMON_UTIL_NONCOPYABLE_HPP