/**
 * @file frame_cache_binding.hpp
 * @brief 驱动期间借用已有帧缓存的守卫，不改变缓存对象的生命周期。
 */
#ifndef FAIO_DETAIL_COROUTINE_MEMORY_FRAME_CACHE_BINDING_HPP
#define FAIO_DETAIL_COROUTINE_MEMORY_FRAME_CACHE_BINDING_HPP

#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/coroutine/memory/frame_allocator.hpp"

namespace faio::detail {
/**  @brief 串行驱动期间安装缓存；退出时先发布剩余批次，再恢复外层 TLS，不清理本地空闲块。 */
class coroutine_frame_cache_binding {
 public:
  /** @brief 保存外层缓存绑定，并安装指定缓存。 */
  explicit coroutine_frame_cache_binding(coroutine_frame_cache& cache) noexcept
      : cache_(cache), previous_(current_thread_context().frames.cache) {
    current_thread_context().frames.cache = &cache_;
  }

  /** @brief 禁止复制缓存绑定守卫。 */
  coroutine_frame_cache_binding(const coroutine_frame_cache_binding&) = delete;
  /** @brief 禁止复制赋值。 */
  coroutine_frame_cache_binding& operator=(const coroutine_frame_cache_binding&) = delete;

  /** @brief 发布剩余批次后恢复外层缓存绑定。 */
  ~coroutine_frame_cache_binding() {
    cache_.flush();
    if (auto* context = try_current_thread_context())
      context->frames.cache = previous_;
  }

 private:
  coroutine_frame_cache& cache_;
  coroutine_frame_cache* previous_;
};
}  // namespace faio::detail
#endif
