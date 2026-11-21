/**
 * @file faio/detail/common/util/cpu_relax.hpp
 * @brief 处理器自旋提示。
 */
#ifndef FAIO_DETAIL_COMMON_UTIL_CPU_RELAX_HPP
#define FAIO_DETAIL_COMMON_UTIL_CPU_RELAX_HPP

#if !((defined(__i386__) || defined(__x86_64__) || defined(__aarch64__)) \
      && (defined(__GNUC__) || defined(__clang__)))
#include <atomic>
#endif

namespace faio::util {
/**
 * @brief 为短暂忙等提供处理器提示，不调用操作系统调度器，也不提供线程间同步。
 * 内存可见性由调用方的原子操作保证；不支持专用指令的平台使用编译器屏障。
 */
inline void cpu_relax() noexcept {
#if (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
  __builtin_ia32_pause();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
  __asm__ __volatile__("yield");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
}  // namespace faio::util
#endif
