#include "frame_allocator_test_types.hpp"

namespace frame_allocator_test {
/** @brief 单独翻译单元中的工厂，测试真实分配而非本地内联消除的帧。 */
faio::task<int> other_tu_value(std::unique_ptr<tracked_payload> payload) {
  co_return payload->value;
}
/** @brief 公开 task 的参数复制在 ramp 中执行，发生异常时尚未进入正常
 * promise/函数体。 */
#if defined(__GNUC__) && !defined(__clang__)
[[gnu::noipa]]
#else
[[gnu::noinline]]
#endif
faio::task<int> other_tu_throwing_parameter(throwing_move_param parameter) {
  co_return parameter.value();
}
/** @brief 将真正挂起的续体交给测试驱动，期间保持帧参数的唯一拥有权。 */
faio::task<int> other_tu_pending_value(std::unique_ptr<tracked_payload> payload,
                                       std::coroutine_handle<> &pending) {
  struct suspend_to_caller {
    std::coroutine_handle<> *continuation;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) const noexcept {
      *continuation = handle;
    }
    void await_resume() const noexcept {}
  };
  co_await suspend_to_caller{&pending};
  co_return payload->value;
}
faio::task<aligned_result> other_tu_aligned_value() {
  co_return aligned_result{};
}
faio::detail::coroutine_frame_cache *other_tu_current_cache() noexcept {
  return faio::detail::current_frame_cache;
}
faio::detail::coroutine_frame_cache **other_tu_tls_address() noexcept {
  return &faio::detail::current_frame_cache;
}
} // namespace frame_allocator_test
