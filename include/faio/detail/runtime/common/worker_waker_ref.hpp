/**
 * @file faio/detail/runtime/common/worker_waker_ref.hpp
 * @brief 具体唤醒服务的单指针静态借用。
 */
#pragma once
#include <concepts>
#include <cstddef>
#include <memory>

namespace faio::io::detail {
/** @brief I/O 资源与操作的归属域。 */
class io_domain;
}  // namespace faio::io::detail

namespace faio::runtime::detail {
/** @brief 独立于 timer/runtime 组合层的 IO 唤醒端，复用所属域的通知合并协议。 */
class io_worker_waker {
 public:
  /** @brief 构造 I/O 工作线程唤醒接口。 */
  explicit io_worker_waker(io::detail::io_domain& domain) noexcept : domain_(&domain) {}

  /** @brief 唤醒工作线程处理已发布工作。 */
  void wake_up() const noexcept;

 private:
  io::detail::io_domain* domain_;
};

/** @brief 运行时工作线程唤醒接口。 */
template <class Derived>
class worker_waker_api {
 public:
  /** @brief 唤醒工作线程处理已发布工作。 */
  void wake_up(this const Derived& self) noexcept {
    if (auto* object = self.native_handle())
      object->wake_up();
  }
};

/** @brief 所有注册槽位保留同一具体 W 类型；不拥有唤醒对象、不保存操作回调。 */
template <class W>
class basic_worker_waker_ref : public worker_waker_api<basic_worker_waker_ref<W>> {
 public:
  /** @brief 构造静态工作线程唤醒引用。 */
  basic_worker_waker_ref() noexcept = default;

  /** @brief 构造静态工作线程唤醒引用。 */
  explicit basic_worker_waker_ref(W& object) noexcept
    requires requires(W& candidate) {
      { candidate.wake_up() } noexcept -> std::same_as<void>;
    }
      : object_(std::addressof(object)) {}

  /** @brief 借用底层原生句柄。 */
  W* native_handle() const noexcept { return object_; }

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return object_ != nullptr; }

 private:
  W* object_{};
};

/** @brief 构造工作线程唤醒引用。 */
template <class W>
basic_worker_waker_ref(W&) -> basic_worker_waker_ref<W>;

/** @brief 指定工作线程的唤醒接口。 */
template <class Derived>
class target_waker_api {
 public:
  /** @brief 唤醒指定工作线程。 */
  void wake_worker(this const Derived& self, std::size_t worker) noexcept {
    if (auto* object = self.native_handle())
      object->wake_worker(worker);
  }
};

/** @brief 宿主借用具体调度域，目标线程编号仍由宿主发布协议决定。 */
template <class W>
class basic_target_waker_ref : public target_waker_api<basic_target_waker_ref<W>> {
 public:
  /** @brief 构造指定工作线程的静态唤醒引用。 */
  basic_target_waker_ref() noexcept = default;

  /** @brief 构造指定工作线程的静态唤醒引用。 */
  explicit basic_target_waker_ref(W& object) noexcept
    requires requires(W& candidate) {
      { candidate.wake_worker(std::size_t{}) } noexcept -> std::same_as<void>;
    }
      : object_(std::addressof(object)) {}

  /** @brief 借用底层原生句柄。 */
  W* native_handle() const noexcept { return object_; }

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return object_ != nullptr; }

 private:
  W* object_{};
};
/** @brief 构造指定工作线程的唤醒引用。 */
template <class W>
basic_target_waker_ref(W&) -> basic_target_waker_ref<W>;
}  // namespace faio::runtime::detail
