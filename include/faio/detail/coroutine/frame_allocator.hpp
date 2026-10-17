/**
 * @file faio/detail/coroutine/frame_allocator.hpp
 * @brief 协程帧、任务上下文与并发组合：协程帧分配与线程缓存。
 * @details 协程句柄的拥有、借用及转交由各类型明确约定。结果发布、停止传播和任务组计数须覆盖挂起、恢复及异常退出，发布恢复句柄后不再访问可能已销毁的帧。
 */
#ifndef FAIO_DETAIL_COROUTINE_FRAME_ALLOCATOR_HPP
#define FAIO_DETAIL_COROUTINE_FRAME_ALLOCATOR_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

/// @brief Sanitizer 接口仅在相应工具链启用时存在，普通发行构建不增加调用或依赖。
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
/// @brief 帧缓存仅覆盖 128..16384 字节的 8 个尺寸级，每级最多保留 4 块。
inline constexpr std::size_t frame_cache_min_block = 128;
inline constexpr std::size_t frame_cache_bin_count = 8;
inline constexpr std::size_t frame_cache_bin_capacity = 4;
inline constexpr std::size_t frame_cache_max_block = frame_cache_min_block
                                                     << (frame_cache_bin_count - 1);
inline constexpr std::size_t frame_cache_max_retained =
    frame_cache_bin_capacity * frame_cache_min_block * ((1u << frame_cache_bin_count) - 1u);
static_assert(frame_cache_max_retained <= 256 * 1024);

/** @brief 独立于协程帧对象的分配记录，始终位于用户帧地址之前。
 * @details 仅保存 global allocation
 * 原地址、原始字节数与尺寸级，不保存线程地址。 元数据与回收链表位于 ASAN
 * payload poison 范围之外；跨线程销毁 和 sized/unsized delete
 * 都可从同一记录找到正确的原始分配。
 */
struct alignas(std::max_align_t) frame_allocation_header {
  void* allocation;  ///< 必须交给 ::operator delete 的原始块地址。
  frame_allocation_header* next{};  ///< 只在完整 destroy 后加入当前线程的缓存链。
  std::size_t allocated_bytes;  ///< 原始块的精确范围，包含对齐填充与尺寸级空隙。
  std::uint8_t bin;  ///< 0xff 表示精确/过对齐分配，不允许进入缓存。
};

inline constexpr std::uint8_t uncached_frame_bin = 0xff;
static_assert(sizeof(frame_allocation_header) % alignof(std::max_align_t) == 0);
/// @brief 元数据与 frame 之间保留对齐大小的 redzone，ASAN 可观察紧邻帧的下溢。
inline constexpr std::size_t frame_allocation_redzone = 16;
inline constexpr std::size_t frame_allocation_prefix =
    sizeof(frame_allocation_header) + frame_allocation_redzone;
static_assert(frame_allocation_prefix % alignof(std::max_align_t) == 0);

/** @brief 运行时驱动作用域拥有的有界缓存；所有原始块仍来自 global new。
 * @details 缓存本体不是 TLS 对象。已分配但尚未 destroy 的帧不在本缓存内，
 *          因此离开作用域只清理空闲块，不影响仍由 task/awaiter 拥有的帧。
 *          普通共享内存生命周期通过现有协程独占所有权保持，无需原线程计数。
 */
class coroutine_frame_cache {
 public:
  coroutine_frame_cache() noexcept = default;

  coroutine_frame_cache(const coroutine_frame_cache&) = delete;

  coroutine_frame_cache& operator=(const coroutine_frame_cache&) = delete;

  ~coroutine_frame_cache() { clear(); }

  /** @brief 领取完整销毁的空闲块，仅解除本次实际 frame 字节的 poison。 */
  frame_allocation_header* acquire(std::size_t bin,
                                   [[maybe_unused]] std::size_t frame_bytes) noexcept {
    /// @brief 从所属线程对应大小档的空闲链借用一个帧块头，不访问其他线程缓存。
    auto* header = heads_[bin];
    /// @brief 缓存档为空时返回未命中，让分配入口按普通拥有型分配路径继续。
    if (!header)
      return nullptr;
    /// @brief 从该档空闲链摘除头块，当前调用者独占它以执行复用或最终释放。
    heads_[bin] = header->next;
    /// @brief 已分配帧不再属于缓存链，清除旧空闲后继避免误回收整条链。
    header->next = nullptr;
    /// @brief 成功领取一块后减少该档缓存数量，保持链表与容量统计一致。
    --counts_[bin];
#if defined(FAIO_DETAIL_FRAME_ASAN)
    /// @brief 跳过固定分配头前缀，取得交给编译器使用的协程帧地址。
    auto* frame = reinterpret_cast<std::byte*>(header) + frame_allocation_prefix;
    /// @brief redzone/级别尾部继续 poison。
    __asan_unpoison_memory_region(frame,
                                  frame_bytes);
#endif
    return header;
  }

  /** @brief 保留已结束帧的原始块；超出单级界限立即释放给 global delete。 */
  bool retain(frame_allocation_header* header) noexcept {
    /// @brief 读取原分配记录的大小档，释放方不根据当前协程类型猜测分配大小。
    const auto bin = header->bin;
    /// @brief 不属于缓存档或该档已满时拒绝留存，外层释放入口继续归还通用分配器。
    if (bin >= frame_cache_bin_count || counts_[bin] == frame_cache_bin_capacity)
      return false;
#if defined(FAIO_DETAIL_FRAME_ASAN)
    /// @brief header/next 始终可读；整个帧及级别余量都 poison，不能掩盖帧销毁后的 UAF。
    __asan_poison_memory_region(header + 1, block_size(bin) - sizeof(*header));
#endif
    /// @brief 把已结束帧块接到本线程空闲链头，随后帧内容不再属于用户协程。
    header->next = heads_[bin];
    /// @brief 让缓存接管该空闲块，只有本线程分配路径才能再次领取。
    heads_[bin] = header;
    /// @brief 每接纳一块空闲存储增加对应档计数，限制线程缓存高水位。
    ++counts_[bin];
    return true;
  }

  /** @brief 回收全部空闲块；离开 runtime 驱动边界时不留下线程级堆保留。 */
  void clear() noexcept {
    /// @brief 逐个大小档回收缓存空闲块，活动协程帧不在这些链中。
    for (std::size_t bin = 0; bin < frame_cache_bin_count; ++bin) {
      /// @brief 连续取出当前档空闲链头，直到该档完全排空。
      while (auto* header = heads_[bin]) {
        /// @brief 从该档空闲链摘除头块，当前调用者独占它以执行复用或最终释放。
        heads_[bin] = header->next;
        /// @brief 保留最初通用分配器返回的基址，最终释放不能使用对齐后的帧指针替代。
        const auto allocation = header->allocation;
#if defined(FAIO_DETAIL_FRAME_ASAN)
        /// @brief 地址检测构建在释放整块存储前撤销缓存毒化，避免分配器访问被误报。
        __asan_unpoison_memory_region(allocation, header->allocated_bytes);
#endif
        /// @brief 元数据生命周期结束后释放原地址，绝不 delete 内部帧地址。
        std::destroy_at(header);
        /// @brief 将不再缓存的原始分配基址归还通用分配器，与最初分配方式配对。
        ::operator delete(allocation);
      }
      /// @brief 该档空闲链已排空，将缓存计数同步清零。
      counts_[bin] = 0;
    }
  }

  static constexpr std::size_t block_size(std::size_t bin) noexcept {
    return frame_cache_min_block << bin;
  }

  /** @brief 当前保留的原始字节数，含 header 与尺寸级空隙，受固定上界约束。 */
  std::size_t retained_bytes() const noexcept {
    /// @brief 从零累计各档实际保留的空闲块字节数，不包含正在使用的协程帧。
    std::size_t bytes{};
    for (std::size_t bin = 0; bin < frame_cache_bin_count; ++bin)
      /// @brief 按档内块数乘实际块跨度累加缓存占用。
      bytes += counts_[bin] * block_size(bin);
    return bytes;
  }

 private:
  std::array<frame_allocation_header*, frame_cache_bin_count> heads_{};  ///< 按大小分档的协程帧空闲链表头，仅由当前线程访问。
  std::array<std::uint8_t, frame_cache_bin_count> counts_{};  ///< 各大小档缓存的帧块数，用于限制线程缓存占用。
};

/// @brief 仅借用当前栈作用域，平凡 TLS 指针没有 allocator 析构或原线程拥有责任。
inline thread_local coroutine_frame_cache* current_frame_cache{};

/** @brief worker/current_thread 驱动入口启用缓存，退出先恢复外层指针再清空。
 * @details cache_ 随运行栈析构，正常线程退出及异常退出都先撤去 TLS 借用；
 *          之后的静态/TLS task 析构走 global fallback。嵌套驱动恢复外层缓存。
 */
class coroutine_frame_cache_scope {
 public:
  coroutine_frame_cache_scope() noexcept : previous_(current_frame_cache) {
    /// @brief 在当前执行作用域安装线程私有帧缓存，退出作用域时恢复外层缓存绑定。
    current_frame_cache = &cache_;
  }

  coroutine_frame_cache_scope(const coroutine_frame_cache_scope&) = delete;

  coroutine_frame_cache_scope& operator=(const coroutine_frame_cache_scope&) = delete;

  ~coroutine_frame_cache_scope() { current_frame_cache = previous_; }

  coroutine_frame_cache& cache() noexcept { return cache_; }

 private:
  coroutine_frame_cache cache_;  ///< 独占已回收原始块，成员析构释放全部。
  coroutine_frame_cache* previous_;  ///< 外层栈作用域在内层执行期间始终存活。
};

/** @brief 为完整协程帧分配原始 storage，按 promise 对齐保留独立 header。
 * @details 默认对齐的小帧可复用当前缓存；超界或过对齐帧直接分配精确原始块。
 *          使用 global new 提供 storage，再在块内对齐；所有退出都 global delete
 *          原地址，不依赖平台 aligned-delete 选择或 deallocation 的 size 参数。
 *          C++23 只向此分配协议传 size，promise 的已知 alignment 可完整保证；
 *          单独 over-aligned 局部对象的对齐传播由编译器协程 lowering 负责。
 */
inline void* allocate_coroutine_frame(std::size_t bytes, std::size_t alignment) {
  /// @brief 至少满足基本最大对齐，更高对齐要求继续保留给后续对齐处理。
  alignment = alignment < alignof(std::max_align_t) ? alignof(std::max_align_t) : alignment;
  /// @brief std::align 只接受有效的幂次对齐，错误不能进入原始块处理。
  if (!std::has_single_bit(alignment))
    throw std::bad_alloc{};

  /// @brief 只为额外对齐预留最坏填充量，普通对齐帧可直接使用固定大小档缓存。
  const auto padding = alignment > alignof(std::max_align_t) ? alignment - 1 : 0;
  if (bytes > std::numeric_limits<std::size_t>::max() - frame_allocation_prefix - padding)
    throw std::bad_alloc{};
  /// @brief 把帧正文、固定头与最坏对齐填充合并为实际分配需求，计算前已检查溢出。
  const auto total = bytes + frame_allocation_prefix + padding;
  /// @brief 默认标记不进入缓存，只有满足大小与对齐约束的帧才分配有效档号。
  auto bin = uncached_frame_bin;
  /// @brief 先采用精确分配跨度，缓存路径可能将其扩大到对应档的固定块大小。
  auto allocated_bytes = total;
  /// @brief 只有当前线程安装缓存、无需额外填充且大小受限的帧才进入缓存热路径。
  if (current_frame_cache && padding == 0 && total <= frame_cache_max_block) {
    /// @brief 把过小帧需求提升到最小缓存块大小，避免产生无意义的更小档。
    const auto rounded = total < frame_cache_min_block ? frame_cache_min_block : total;
    bin = static_cast<std::uint8_t>(std::bit_width(rounded - 1)
                                    - std::countr_zero(frame_cache_min_block));
    /// @brief 对应档命中时直接取得已有空闲块，避免再次调用通用分配器。
    if (auto* header = current_frame_cache->acquire(bin, bytes))
      return reinterpret_cast<std::byte*>(header) + frame_allocation_prefix;
    /// @brief 缓存未命中时按整档块大小分配，使将来回收到同档时跨度一致。
    allocated_bytes = coroutine_frame_cache::block_size(bin);
  }
  /// @brief 仅 allocator 内部处理原始 storage。
  void* allocation = ::operator new(allocated_bytes);
  /// @brief 先跳过头部前缀作为候选帧地址，再在预留空间内满足实际对齐。
  void* frame = static_cast<std::byte*>(allocation) + frame_allocation_prefix;
  /// @brief 只把头部之后的空间交给 std::align，避免对齐结果覆盖分配记录。
  auto remaining = allocated_bytes - frame_allocation_prefix;
  /// @brief 对齐无法在已分配范围内满足时释放原始块并报告分配失败。
  if (!std::align(alignment, bytes, frame, remaining)) {
    /// @brief 对齐失败也保持分配/释放配对，不泄漏原块。
    ::operator delete(allocation);
    throw std::bad_alloc{};
  }
  auto* header = reinterpret_cast<frame_allocation_header*>(static_cast<std::byte*>(frame)
                                                            - frame_allocation_prefix);
  /// @brief 在对齐帧前构造回收记录，保存原始基址、实际跨度与缓存档号。
  std::construct_at(header, frame_allocation_header{allocation, nullptr, allocated_bytes, bin});
#if defined(FAIO_DETAIL_FRAME_ASAN)
  /// @brief 新 global 块也必须保持帧的精确边界：对齐填充、redzone、级别尾部均不可读。
  __asan_poison_memory_region(allocation, allocated_bytes);
  /// @brief 仅元数据始终可访问。
  __asan_unpoison_memory_region(header,
                                sizeof(*header));
  /// @brief 帧起点按8字节以上对齐，支持尾部partial shadow。
  __asan_unpoison_memory_region(frame, bytes);
#endif
  return frame;
}

/** @brief 编译器完整 destroy 后才回收 storage，sized/unsized 共用同一拥有记录。
 */
inline void release_coroutine_frame(void* frame) noexcept {
  /// @brief 空帧释放保持无操作，不反向计算或解引用分配头。
  if (!frame)
    return;
  auto* header = reinterpret_cast<frame_allocation_header*>(static_cast<std::byte*>(frame)
                                                            - frame_allocation_prefix);
  /// @brief 可进入销毁线程缓存；记录中没有原线程的借用地址。
  if (current_frame_cache && current_frame_cache->retain(header))
    return;
  /// @brief 保留最初通用分配器返回的基址，最终释放不能使用对齐后的帧指针替代。
  const auto allocation = header->allocation;
  /// @brief 销毁分配头前保存检测工具可能需要的整块跨度。
  [[maybe_unused]] const auto allocated_bytes = header->allocated_bytes;
  /// @brief 已不再需要分配元数据，先结束头对象生命期再归还原始存储。
  std::destroy_at(header);
#if defined(FAIO_DETAIL_FRAME_ASAN)
  /// @brief 原始 global delete 接管整块。
  __asan_unpoison_memory_region(allocation, allocated_bytes);
#endif
  /// @brief 将不再缓存的原始分配基址归还通用分配器，与最初分配方式配对。
  ::operator delete(allocation);
}

/** @brief task promise 的标准分配协议，CRTP 延迟查询完整 promise 的实际对齐。
 * @details 未启动任务析构、正常结果领取、异常构造与异常传播均由编译器/现有
 *          task 帧所有者调用相同
 * delete。只有销毁责任方能回收，不增加挂起帧回收。
 */
template <class Promise>
struct task_frame_allocation {
  static void* operator new(std::size_t bytes) {
    return allocate_coroutine_frame(bytes, alignof(Promise));
  }

  static void operator delete(void* frame, std::size_t) noexcept { release_coroutine_frame(frame); }

  static void operator delete(void* frame) noexcept { release_coroutine_frame(frame); }
};
}
#endif
