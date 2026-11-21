/**
 * @file large_frame_pool_binding.hpp
 * @brief 驱动期间借用具体的大帧服务，不改变服务的生命周期。
 */
#ifndef FAIO_DETAIL_COROUTINE_MEMORY_LARGE_FRAME_POOL_BINDING_HPP
#define FAIO_DETAIL_COROUTINE_MEMORY_LARGE_FRAME_POOL_BINDING_HPP

#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/coroutine/memory/large_frame_pool.hpp"

namespace faio::detail {
/**  @brief 即使为 nullptr 也显式安装，防止嵌套且禁用页池的运行时误用外层服务。 */
class large_frame_pool_binding {
 public:
  /** @brief 保存外层页池绑定，并安装指定页池。 */
  explicit large_frame_pool_binding(large_frame_pool* pool) noexcept
      : previous_(current_thread_context().frames.large_pool) {
    current_thread_context().frames.large_pool = pool;
  }

  /** @brief 禁止复制页池绑定守卫。 */
  large_frame_pool_binding(const large_frame_pool_binding&) = delete;
  /** @brief 禁止复制赋值。 */
  large_frame_pool_binding& operator=(const large_frame_pool_binding&) = delete;

  /** @brief 退出作用域时恢复外层页池绑定。 */
  ~large_frame_pool_binding() {
    if (auto* context = try_current_thread_context())
      context->frames.large_pool = previous_;
  }

 private:
  large_frame_pool* previous_;
};
}  // namespace faio::detail
#endif
