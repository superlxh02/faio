/**
 * @file faio/detail/net/base/awaiter_options.hpp
 * @brief 网络地址、socket 与传输协议：网络等待超时选项。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#ifndef FAIO_DETAIL_NET_BASE_AWAITER_OPTIONS_HPP
#define FAIO_DETAIL_NET_BASE_AWAITER_OPTIONS_HPP
#include <chrono>
#include <optional>
#include <utility>

namespace faio::net::detail {
/** @brief 组合网络 awaiter 的 deadline 配置；构造和配置都没有后端副作用。
 * @details move 仅发生在提交以前，绝对 deadline 随 awaiter
 * 移动，不延长已设置的超时。
 */
template <class Derived>
class AwaiterOptions {
 public:
  /// @brief 把相对时长转换为单调时钟截止时间并保存到本次等待。
  template <class Rep, class Period>
  auto set_timeout(std::chrono::duration<Rep, Period> duration) & noexcept -> Derived& {
    return set_timeout_at(std::chrono::steady_clock::now() + duration);
  }

  /// @brief 把相对时长转换为单调时钟截止时间并保存到本次等待。
  template <class Rep, class Period>
  auto set_timeout(std::chrono::duration<Rep, Period> duration) && noexcept -> Derived&& {
    set_timeout_at(std::chrono::steady_clock::now() + duration);
    return std::move(*static_cast<Derived*>(this));
  }

  /// @brief 保存单调时钟绝对截止时间，真正超时仲裁由提交与驱动协议完成。
  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) & noexcept -> Derived& {
    deadline_ = deadline;
    return *static_cast<Derived*>(this);
  }

  /// @brief 保存单调时钟绝对截止时间，真正超时仲裁由提交与驱动协议完成。
  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) && noexcept -> Derived&& {
    deadline_ = deadline;
    return std::move(*static_cast<Derived*>(this));
  }

 protected:
  template <class Operation>
  void configure(Operation& operation) const noexcept {
    if (deadline_)
      operation.set_timeout_at(*deadline_);
  }

 private:
  std::optional<std::chrono::steady_clock::time_point> deadline_;  ///< 本次操作可选的单调时钟绝对截止时间。
};
}
#endif
