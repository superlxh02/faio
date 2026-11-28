/**
 * @file faio/detail/tls/detail/gate_lease.hpp
 * @brief TLS 读写方向的独占租约与释放通知。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/wait/generation_event.hpp"
#include <atomic>
#include <cerrno>
#include <utility>

namespace faio::tls::detail {
/** @brief 应用操作申请的互斥方向，读写方向可分别持有租约。 */
enum class app_direction {
  READ, /**< 应用读取方向。 */
  WRITE /**< 应用写入方向。 */
};

/** @brief 通过转移所有权管理方向门闩，在所有退出路径自动释放并通知等待者。 */
class gate_lease {
 public:
  /**
   * @brief 接管已获得的门闩；调用者必须先将 acquired 置为占用状态。
   * @param acquired 本租约负责释放的原子门闩，生命周期必须覆盖租约。
   * @param notify 释放时广播的可选事件，生命周期必须覆盖租约。
   */
  explicit gate_lease(std::atomic_bool& acquired,
                      ::faio::detail::generation_event* notify = nullptr) noexcept
      : gate_(&acquired), notify_(notify) {}

  /** @brief 禁止复制，保证同一占用只会被释放一次。 */
  gate_lease(const gate_lease&) = delete;
  /** @brief 禁止复制赋值，保持租约的独占所有权。 */
  auto operator=(const gate_lease&) -> gate_lease& = delete;

  /**
   * @brief 转移释放责任，使原租约成为空对象。
   * @param other 被接管的租约。
   */
  gate_lease(gate_lease&& other) noexcept
      : gate_(std::exchange(other.gate_, nullptr)), notify_(other.notify_) {}

  /**
   * @brief 先释放当前占用，再接管另一个租约。
   * @param other 被接管的租约；支持自身移动赋值。
   * @return 当前租约。
   */
  auto operator=(gate_lease&& other) noexcept -> gate_lease& {
    if (this != &other) {
      release();
      gate_ = std::exchange(other.gate_, nullptr);
      notify_ = other.notify_;
    }
    return *this;
  }

  /** @brief 无异常地释放占用，并唤醒依赖该方向空闲的协程。 */
  ~gate_lease() noexcept { release(); }

  /**
   * @brief 尝试申请方向门闩而不等待，供同方向重入检查使用。
   * @param gate 与所有申请者共享的原子占用位。
   * @param notify 释放占用时需要广播的事件。
   * @return 独占租约；已有占用时返回 EBUSY。
   */
  static auto try_acquire(std::atomic_bool& gate,
                          ::faio::detail::generation_event* notify = nullptr)
      -> expected<gate_lease> {
    bool idle = false;
    if (!gate.compare_exchange_strong(idle, true, std::memory_order_acq_rel))
      return std::unexpected(error{EBUSY});
    return gate_lease{gate, notify};
  }

 private:
  /** @brief 清空自身释放责任，再发布空闲状态，允许安全重复调用。 */
  void release() noexcept {
    if (auto* gate = std::exchange(gate_, nullptr)) {
      gate->store(false, std::memory_order_release);
      if (notify_)
        notify_->notify_all();
    }
  }

  std::atomic_bool* gate_{};                   /**< 借用的方向门闩；空指针表示未持有占用。 */
  ::faio::detail::generation_event* notify_{}; /**< 释放方向后广播的可选事件。 */
};
}  // namespace faio::tls::detail
