#ifndef FAIO_DETAIL_NET_COMMON_AWAITER_OPTIONS_HPP
#define FAIO_DETAIL_NET_COMMON_AWAITER_OPTIONS_HPP
#include <chrono>
#include <optional>
#include <utility>
namespace faio::net::detail {
/** @brief 组合网络 awaiter 的 deadline 配置；构造和配置都没有后端副作用。
 * @details move 仅发生在提交以前，绝对 deadline 随 awaiter
 * 移动，不延长已设置的超时。
 */
template <class Derived> class AwaiterOptions {
public:
  template <class Rep, class Period>
  auto set_timeout(std::chrono::duration<Rep, Period> duration) & noexcept
      -> Derived & {
    return set_timeout_at(std::chrono::steady_clock::now() + duration);
  }
  template <class Rep, class Period>
  auto set_timeout(std::chrono::duration<Rep, Period> duration) && noexcept
      -> Derived && {
    set_timeout_at(std::chrono::steady_clock::now() + duration);
    return std::move(*static_cast<Derived *>(this));
  }
  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) & noexcept
      -> Derived & {
    deadline_ = deadline;
    return *static_cast<Derived *>(this);
  }
  auto
  set_timeout_at(std::chrono::steady_clock::time_point deadline) && noexcept
      -> Derived && {
    deadline_ = deadline;
    return std::move(*static_cast<Derived *>(this));
  }

protected:
  template <class Operation>
  void configure(Operation &operation) const noexcept {
    if (deadline_)
      operation.set_timeout_at(*deadline_);
  }

private:
  std::optional<std::chrono::steady_clock::time_point> deadline_;
};
} // namespace faio::net::detail
#endif
