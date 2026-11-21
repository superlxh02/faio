/**
 * @file faio/detail/coroutine/memory/frame_allocator.hpp
 * @brief 协程帧分配与线程缓存。
 */
#ifndef FAIO_DETAIL_COROUTINE_MEMORY_FRAME_ALLOCATOR_HPP
#define FAIO_DETAIL_COROUTINE_MEMORY_FRAME_ALLOCATOR_HPP

#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/coroutine/memory/large_frame_pool_binding.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FAIO_DETAIL_FRAME_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(FAIO_DETAIL_FRAME_ASAN)
#define FAIO_DETAIL_FRAME_ASAN 1
#endif
#if defined(FAIO_DETAIL_FRAME_ASAN)
#include <sanitizer/asan_interface.h>
#endif

namespace faio::detail {
/** @brief 8 个尺寸档覆盖 128 至 16384 字节，在 256 KiB 上限内优先缓存常见的中小型任务帧。 */
inline constexpr std::size_t frame_cache_min_block = 128;
inline constexpr std::size_t frame_cache_bin_count = 8;
inline constexpr std::size_t frame_cache_bin_capacity = 4;
inline constexpr std::size_t frame_cache_hot_bin = 1;
inline constexpr std::size_t frame_cache_hot_capacity = 64;
inline constexpr std::size_t frame_cache_max_block = frame_cache_min_block
                                                     << (frame_cache_bin_count - 1);
inline constexpr std::array<std::uint8_t, frame_cache_bin_count> frame_cache_bin_capacities{
    4, 64, 32, 64, 16, 4, 4, 4};
inline constexpr std::size_t frame_cache_max_retained = [] {
  std::size_t bytes{};
  for (std::size_t bin = 0; bin < frame_cache_bin_count; ++bin)
    bytes += frame_cache_bin_capacities[bin] * (frame_cache_min_block << bin);
  return bytes;
}();
static_assert(frame_cache_max_retained <= 256 * 1024);

/**
 * @brief 独立于协程帧对象的分配记录，始终位于用户帧地址之前。
 * @details 仅保存全局分配的原地址、原始字节数和尺寸档，不保存线程地址。
 * 元数据与回收链表位于 ASan 对帧正文标记的不可访问区域之外；跨线程销毁、
 * 带大小参数或不带大小参数的 delete 均可通过同一记录找到正确的原始分配。
 */
enum class frame_allocation_kind : std::uint8_t { heap, mapped_extent };

/** @brief 协程帧分配来源与释放信息。 */
struct alignas(std::max_align_t) frame_allocation_header {
  union {
    void* allocation;
    large_frame_extent* extent;
  }; /**< 必须交给 ::operator delete 的原始块地址。 */

  frame_allocation_header* next{}; /**< 仅在协程帧完整销毁后加入当前线程的缓存链。 */
  std::size_t allocated_bytes;     /**< 原始块的精确范围，包含对齐填充与尺寸级空隙。 */
  std::uint8_t bin;                /**< 0xff 表示不进入小帧缓存；释放后端由 kind 决定。 */
  frame_allocation_kind kind{frame_allocation_kind::heap}; /**< 在填充空间中记录释放协议。 */
};

inline constexpr std::uint8_t uncached_frame_bin = 0xff;
static_assert(sizeof(frame_allocation_header) % alignof(std::max_align_t) == 0);
/** @brief 元数据与协程帧之间保留与对齐大小相同的 redzone，使 ASan 可以检测紧邻帧的下溢。 */
inline constexpr std::size_t frame_allocation_redzone = 16;
inline constexpr std::size_t frame_allocation_prefix =
    sizeof(frame_allocation_header) + frame_allocation_redzone;
static_assert(frame_allocation_prefix % alignof(std::max_align_t) == 0);

/**
 * @brief 运行时驱动作用域持有的有界缓存；所有原始块仍来自 global new。
 * @details 缓存本体不是 TLS 对象。已分配但尚未 destroy 的帧不在本缓存内，
 *          因此离开作用域只清理空闲块，不影响仍由 task/awaiter 拥有的帧。
 *          协程的独占所有者维持分配内存的生命周期，无需原线程计数。
 */
class coroutine_frame_cache {
 public:
  /** @brief 构造线程本地协程帧缓存。 */
  coroutine_frame_cache() noexcept = default;

  /** @brief 禁止复制构造线程本地协程帧缓存。 */
  coroutine_frame_cache(const coroutine_frame_cache&) = delete;

  /** @brief 禁止复制赋值。 */
  coroutine_frame_cache& operator=(const coroutine_frame_cache&) = delete;

  /** @brief 销毁线程本地协程帧缓存。 */
  ~coroutine_frame_cache() { clear(); }

  /**  @brief 领取已完整销毁的空闲块，仅解除本次实际帧范围内的 ASan 不可访问标记。 */
  frame_allocation_header* acquire(std::size_t bin,
                                   [[maybe_unused]] std::size_t frame_bytes) noexcept {
    auto* header = heads_[bin];
    if (!header)
      return nullptr;
    heads_[bin] = header->next;
    header->next = nullptr;
    --counts_[bin];
#if defined(FAIO_DETAIL_FRAME_ASAN)
    auto* frame = reinterpret_cast<std::byte*>(header) + frame_allocation_prefix;
    __asan_unpoison_memory_region(frame, frame_bytes);
#endif
    return header;
  }

  /**  @brief 保留已结束协程帧的原始块；超出该尺寸档的上限时，立即通过 global delete 释放。 */
  bool retain(frame_allocation_header* header) noexcept {
    const auto bin = header->bin;
    if (bin >= frame_cache_bin_count || counts_[bin] == bin_capacity(bin))
      return false;
#if defined(FAIO_DETAIL_FRAME_ASAN)
    __asan_poison_memory_region(header + 1, block_size(bin) - sizeof(*header));
#endif
    header->next = heads_[bin];
    heads_[bin] = header;
    ++counts_[bin];
    return true;
  }

  /**  @brief 本地策略没有需要发往其他线程的剩余批次；统一驱动入口可静态调用，不增加热路径工作。 */
  void flush() noexcept {}

  /**  @brief 回收全部空闲块，不影响仍在使用的帧或外层 TLS 借用。 */
  void clear() noexcept {
    for (std::size_t bin = 0; bin < frame_cache_bin_count; ++bin) {
      while (auto* header = heads_[bin]) {
        heads_[bin] = header->next;
        const auto allocation = header->allocation;
#if defined(FAIO_DETAIL_FRAME_ASAN)
        __asan_unpoison_memory_region(allocation, header->allocated_bytes);
#endif
        std::destroy_at(header);
        ::operator delete(allocation);
      }
      counts_[bin] = 0;
    }
  }

  /**  @brief 容量限制仅计算空闲块，活动帧不占额度；热路径不维护总字节计数，也不执行共享 CAS。 */
  static constexpr std::size_t bin_capacity(std::size_t bin) noexcept {
    return frame_cache_bin_capacities[bin];
  }

  /** @brief 读取存储块大小。 */
  static constexpr std::size_t block_size(std::size_t bin) noexcept {
    return frame_cache_min_block << bin;
  }

  /**  @brief 当前保留的原始字节数，含 header 与尺寸级空隙，受固定上界约束。 */
  std::size_t retained_bytes() const noexcept {
    std::size_t bytes{};
    for (std::size_t bin = 0; bin < frame_cache_bin_count; ++bin)
      bytes += counts_[bin] * block_size(bin);
    return bytes;
  }

 private:
  std::array<frame_allocation_header*, frame_cache_bin_count>
      heads_{}; /**< 按大小分档的协程帧空闲链表头，仅由当前线程访问。 */
  std::array<std::uint8_t, frame_cache_bin_count>
      counts_{}; /**< 各大小档缓存的帧块数，用于限制线程缓存占用。 */
};

/**
 * @brief 在 worker 或 current_thread 驱动入口启用缓存，退出时先恢复外层指针再清空缓存。
 * @details cache_ 随运行栈析构，正常线程退出及异常退出都先撤去 TLS 借用；
 *          此后静态对象和 TLS 中的 task 析构时回退到全局释放路径。嵌套驱动退出时恢复外层缓存。
 */
class coroutine_frame_cache_scope {
 public:
  /** @brief 构造协程帧缓存作用域。 */
  coroutine_frame_cache_scope() noexcept : previous_(current_thread_context().frames.cache) {
    current_thread_context().frames.cache = &cache_;
  }

  /** @brief 禁止复制构造协程帧缓存作用域。 */
  coroutine_frame_cache_scope(const coroutine_frame_cache_scope&) = delete;

  /** @brief 禁止复制赋值。 */
  coroutine_frame_cache_scope& operator=(const coroutine_frame_cache_scope&) = delete;

  /** @brief 销毁协程帧缓存作用域。 */
  ~coroutine_frame_cache_scope() {
    if (auto* context = try_current_thread_context())
      context->frames.cache = previous_;
  }

  /** @brief 取得协程帧缓存。 */
  coroutine_frame_cache& cache() noexcept { return cache_; }

 private:
  coroutine_frame_cache cache_;     /**< 独占已回收原始块，成员析构释放全部。 */
  coroutine_frame_cache* previous_; /**< 外层栈作用域在内层执行期间始终存活。 */
};

/**
 * @brief 为完整协程帧分配原始存储，并按 promise 的对齐要求保留独立分配头。
 * @details 默认对齐的小帧可复用当前缓存；超界或过对齐帧直接分配精确原始块。
 *          通过 global new 分配存储，再在块内对齐；所有释放路径均将原地址交给 global delete，
 *          不依赖平台对 aligned delete 的选择或释放时传入的 size 参数。
 *          C++23 仅向此分配协议传入 size，这里可以满足 promise 的已知对齐要求；
 *          具有更高对齐要求的局部对象，其对齐传播由编译器的协程转换负责。
 */
inline void* allocate_coroutine_frame(std::size_t bytes, std::size_t alignment) {
  if (!std::has_single_bit(alignment))
    throw std::bad_alloc{};
  alignment = alignment < alignof(std::max_align_t) ? alignof(std::max_align_t) : alignment;

  const auto padding = alignment > alignof(std::max_align_t) ? alignment - 1 : 0;
  if (bytes > std::numeric_limits<std::size_t>::max() - frame_allocation_prefix - padding)
    throw std::bad_alloc{};
  const auto total = bytes + frame_allocation_prefix + padding;
  auto bin = uncached_frame_bin;
  auto allocated_bytes = total;
  const auto* thread = try_current_thread_context();
  auto* frame_cache = thread ? thread->frames.cache : nullptr;
  if (frame_cache && padding == 0 && total <= frame_cache_max_block) {
    const auto rounded = total < frame_cache_min_block ? frame_cache_min_block : total;
    bin = static_cast<std::uint8_t>(std::bit_width(rounded - 1)
                                    - std::countr_zero(frame_cache_min_block));
    if (auto* header = frame_cache->acquire(bin, bytes))
      return reinterpret_cast<std::byte*>(header) + frame_allocation_prefix;
    allocated_bytes = coroutine_frame_cache::block_size(bin);
  }
  if (bytes >= large_frame_pool_min_frame_bytes && thread && thread->frames.large_pool) {
    if (auto* extent =
            thread->frames.large_pool->try_acquire(bytes, frame_allocation_prefix, alignment)) {
      auto* frame = static_cast<std::byte*>(extent->frame_address());
      auto* header = reinterpret_cast<frame_allocation_header*>(frame - frame_allocation_prefix);
      auto* original = static_cast<std::byte*>(extent->mapping_address());
      const auto available =
          extent->mapping_bytes()
          - static_cast<std::size_t>(reinterpret_cast<std::byte*>(header) - original);
      std::construct_at(header,
                        frame_allocation_header{.extent = extent,
                                                .next = nullptr,
                                                .allocated_bytes = available,
                                                .bin = uncached_frame_bin,
                                                .kind = frame_allocation_kind::mapped_extent});
#if defined(FAIO_DETAIL_FRAME_ASAN)
      __asan_poison_memory_region(original, extent->mapping_bytes());
      __asan_unpoison_memory_region(header, sizeof(*header));
      __asan_unpoison_memory_region(frame, bytes);
#endif
      return frame;
    }
  }
  void* allocation = ::operator new(allocated_bytes);
  void* frame = static_cast<std::byte*>(allocation) + frame_allocation_prefix;
  auto remaining = allocated_bytes - frame_allocation_prefix;
  if (!std::align(alignment, bytes, frame, remaining)) {
    ::operator delete(allocation);
    throw std::bad_alloc{};
  }
  auto* header = reinterpret_cast<frame_allocation_header*>(static_cast<std::byte*>(frame)
                                                            - frame_allocation_prefix);
  std::construct_at(header,
                    frame_allocation_header{.allocation = allocation,
                                            .next = nullptr,
                                            .allocated_bytes = allocated_bytes,
                                            .bin = bin,
                                            .kind = frame_allocation_kind::heap});
#if defined(FAIO_DETAIL_FRAME_ASAN)
  __asan_poison_memory_region(allocation, allocated_bytes);
  __asan_unpoison_memory_region(header, sizeof(*header));
  __asan_unpoison_memory_region(frame, bytes);
#endif
  return frame;
}

/**  @brief 编译器完整销毁协程后才回收存储；带大小参数和不带大小参数的释放入口共用同一分配记录。 */
inline void release_coroutine_frame(void* frame) noexcept {
  if (!frame)
    return;
  auto* header = reinterpret_cast<frame_allocation_header*>(static_cast<std::byte*>(frame)
                                                            - frame_allocation_prefix);
  const auto* thread = try_current_thread_context();
  auto* frame_cache = thread ? thread->frames.cache : nullptr;
  if (frame_cache && frame_cache->retain(header))
    return;
  if (header->kind == frame_allocation_kind::mapped_extent) {
    auto* extent = header->extent;
    std::destroy_at(header);
    large_frame_pool::release(extent);
    return;
  }
  const auto allocation = header->allocation;
  [[maybe_unused]] const auto allocated_bytes = header->allocated_bytes;
  std::destroy_at(header);
#if defined(FAIO_DETAIL_FRAME_ASAN)
  __asan_unpoison_memory_region(allocation, allocated_bytes);
#endif
  ::operator delete(allocation);
}

/**
 * @brief task promise 的标准分配协议，通过 CRTP 延迟查询完整 promise 类型的实际对齐要求。
 * @details 未启动任务析构、正常领取结果、构造失败和异常传播均由编译器或现有 task 帧所有者
 *          调用同一 delete。只有负责销毁的一方可以回收存储，不额外回收挂起中的帧。
 */
template <class Promise>
struct task_frame_allocation {
#if !defined(FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW) || !FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW
  /** @brief 分配协程帧存储。 */
  static void* operator new(std::size_t bytes) {
    return allocate_coroutine_frame(bytes, alignof(Promise));
  }

  /** @brief 释放协程帧存储。 */
  static void operator delete(void* frame, std::size_t) noexcept { release_coroutine_frame(frame); }

  /** @brief 释放协程帧存储。 */
  static void operator delete(void* frame) noexcept { release_coroutine_frame(frame); }
#endif
};
}  // namespace faio::detail
#endif
