/**
 * @file faio/detail/io/testing/control_ref.hpp
 * @brief 具体测试策略的单指针静态借用。
 */
#pragma once
#include <memory>
#include <utility>

namespace faio::io::detail {
/** @brief I/O 控制唤醒策略接口。 */
template <class Derived>
class control_policy_api {
 public:
  /** @brief 调用测试策略，处理指定测试点。 */
  template <class Point>
  void step(this const Derived& self, Point point) noexcept {
    if (auto* policy = self.native_handle())
      policy->step(point);
  }

  /** @brief 通过测试策略提交已准备的原生请求。 */
  template <class... Args>
  decltype(auto) submit(this const Derived& self, Args&&... args) noexcept {
    return self.native_handle()->submit(std::forward<Args>(args)...);
  }

  /** @brief 通过测试策略执行原生队列等待。 */
  decltype(auto) wait(this const Derived& self) noexcept { return self.native_handle()->wait(); }

  /** @brief 通过测试策略写入控制通知。 */
  template <class... Args>
  decltype(auto) write(this const Derived& self, Args&&... args) noexcept {
    return self.native_handle()->write(std::forward<Args>(args)...);
  }
};

/** @brief 控制唤醒策略的静态借用引用。 */
template <class Policy>
class basic_control_policy_ref : public control_policy_api<basic_control_policy_ref<Policy>> {
 public:
  /** @brief 构造控制唤醒策略的静态借用引用。 */
  basic_control_policy_ref() = default;

  /** @brief 构造控制唤醒策略的静态借用引用。 */
  explicit basic_control_policy_ref(Policy& policy) noexcept : policy_(std::addressof(policy)) {}

  /** @brief 借用具体测试策略对象。 */
  Policy* native_handle() const noexcept { return policy_; }

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return policy_ != nullptr; }

 private:
  Policy* policy_{};
};
}  // namespace faio::io::detail
