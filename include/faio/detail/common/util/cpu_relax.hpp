#ifndef FAIO_DETAIL_COMMON_UTIL_CPU_RELAX_HPP
#define FAIO_DETAIL_COMMON_UTIL_CPU_RELAX_HPP

#include <atomic>

namespace faio::util {
// 短暂忙等使用的处理器提示，不调用操作系统调度器，也不承担线程间同步。
// 内存可见性由调用方的原子操作保证；不支持专用指令的平台使用编译器屏障。
inline void cpu_relax() noexcept {
#if (defined(__i386__) || defined(__x86_64__)) &&                              \
    (defined(__GNUC__) || defined(__clang__))
  __builtin_ia32_pause();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
  __asm__ __volatile__("yield");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
} // namespace faio::util
#endif // FAIO_DETAIL_COMMON_UTIL_CPU_RELAX_HPP
