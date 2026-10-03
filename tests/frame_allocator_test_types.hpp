#pragma once
#include "faio/detail/coroutine/task.hpp"
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace frame_allocator_test {
/** @brief 真实帧参数拥有的 payload；销毁计数由帧所有权归还而不是测试替身递增。
 */
struct tracked_payload {
  std::atomic<unsigned> &destroyed;
  int value{42};
  explicit tracked_payload(std::atomic<unsigned> &counter) noexcept
      : destroyed(counter) {}
  ~tracked_payload() { destroyed.fetch_add(1, std::memory_order_relaxed); }
};
/** @brief 由调用者拥有的构造观察器；计数和地址值可区分 caller copy 与 ramp
 * move。 */
struct parameter_construction_probe {
  std::atomic<unsigned> payload_destroyed{0};
  unsigned copies{}, move_attempts{}, completed{}, destructors{};
  std::uintptr_t copy_source{}, copy_target{}, move_source{}, move_target{};
};
/** @brief caller 左值复制拥有独立 payload，只有帧内 xvalue
 * 构造按标志抛测试异常。 */
class throwing_move_param {
public:
  explicit throwing_move_param(parameter_construction_probe &probe, bool fail)
      : probe_(&probe),
        payload_(std::make_unique<tracked_payload>(probe.payload_destroyed)),
        fail_(fail) {
    ++probe_->completed;
  }
  throwing_move_param(const throwing_move_param &other)
      : probe_(other.probe_),
        payload_(std::make_unique<tracked_payload>(probe_->payload_destroyed)),
        fail_(other.fail_) {
    ++probe_->copies; // caller 参数 copy 不主动抛测试异常，完整建立自己的
                      // payload。
    ++probe_->completed;
    probe_->copy_source = reinterpret_cast<std::uintptr_t>(&other);
    probe_->copy_target = reinterpret_cast<std::uintptr_t>(this);
  }
  throwing_move_param(throwing_move_param &&other)
      : probe_(other.probe_), fail_(other.fail_) {
    ++probe_->move_attempts; // 未完成构造也记录，不能靠完整对象的析构计数反推。
    probe_->move_source = reinterpret_cast<std::uintptr_t>(&other);
    probe_->move_target = reinterpret_cast<std::uintptr_t>(this);
    if (fail_)
      throw std::runtime_error(
          "frame parameter move failed"); // 此时 caller copy 仍拥有 payload。
    payload_ = std::move(other.payload_);
    ++probe_->completed;
  }
  throwing_move_param &operator=(const throwing_move_param &) = delete;
  throwing_move_param &operator=(throwing_move_param &&) = delete;
  ~throwing_move_param() {
    ++probe_->destructors;
  } // unique_ptr 成员随后归还实际 heap payload。
  int value() const noexcept { return payload_->value; }

private:
  parameter_construction_probe
      *probe_; ///< 观察器始终活过 caller 与失败帧的异常展开。
  std::unique_ptr<tracked_payload> payload_;
  bool fail_;
};
/** @brief 返回槽要求 128 字节对齐，从而 promise 也具有确定的过对齐要求。 */
struct alignas(128) aligned_result {
  int value{42};
};
faio::task<int> other_tu_value(std::unique_ptr<tracked_payload> payload);
faio::task<int> other_tu_throwing_parameter(throwing_move_param parameter);
faio::task<int> other_tu_pending_value(std::unique_ptr<tracked_payload> payload,
                                       std::coroutine_handle<> &pending);
faio::task<aligned_result> other_tu_aligned_value();
faio::detail::coroutine_frame_cache *other_tu_current_cache() noexcept;
faio::detail::coroutine_frame_cache **other_tu_tls_address() noexcept;
} // namespace frame_allocator_test
