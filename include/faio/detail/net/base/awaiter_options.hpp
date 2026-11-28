/**
 * @file faio/detail/net/base/awaiter_options.hpp
 * @brief 网络等待超时选项。
 */
#ifndef FAIO_DETAIL_NET_BASE_AWAITER_OPTIONS_HPP
#define FAIO_DETAIL_NET_BASE_AWAITER_OPTIONS_HPP
#include <chrono>
#include <optional>
#include <utility>

namespace faio::net::detail {
/**
 *  @brief 为网络 awaiter 配置 deadline；构造和配置都没有后端副作用。
 * @details 仅允许在提交前 move；绝对 deadline 随 awaiter
 * 移动，不延长已设置的超时。
 */
template <class Derived>
class awaiter_options {
 public:
  /** @brief 将相对时长转换为单调时钟截止时间，保存到本次等待中。 */
  template <class Rep, class Period>
  auto set_timeout(std::chrono::duration<Rep, Period> duration) & noexcept -> Derived& {
    return set_timeout_at(std::chrono::steady_clock::now() + duration);
  }

  /** @brief 将相对时长转换为单调时钟截止时间，保存到本次等待中。 */
  template <class Rep, class Period>
  auto set_timeout(std::chrono::duration<Rep, Period> duration) && noexcept -> Derived&& {
    set_timeout_at(std::chrono::steady_clock::now() + duration);
    return std::move(*static_cast<Derived*>(this));
  }

  /** @brief 保存单调时钟的绝对截止时间，由提交与驱动协议仲裁超时。 */
  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) & noexcept -> Derived& {
    deadline_ = deadline;
    return *static_cast<Derived*>(this);
  }

  /** @brief 保存单调时钟的绝对截止时间，由提交与驱动协议仲裁超时。 */
  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) && noexcept -> Derived&& {
    deadline_ = deadline;
    return std::move(*static_cast<Derived*>(this));
  }

 protected:
  /** @brief 把当前等待选项传递给底层 IO 操作。 */
  template <class Operation>
  void configure(Operation& operation) const noexcept {
    if (deadline_)
      operation.set_timeout_at(*deadline_);
  }

 private:
  std::optional<std::chrono::steady_clock::time_point>
      deadline_; /**< 本次操作可选的单调时钟绝对截止时间。 */
};
}  // namespace faio::net::detail
#endif
