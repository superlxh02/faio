/**
 * @file large_frame_pool.hpp
 * @brief 运行时可选的大协程帧页池；默认关闭，不接管小帧或全局 new。
 */
#ifndef FAIO_DETAIL_COROUTINE_MEMORY_LARGE_FRAME_POOL_HPP
#define FAIO_DETAIL_COROUTINE_MEMORY_LARGE_FRAME_POOL_HPP

#include <atomic>
#include <bit>
#include <cassert>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FAIO_DETAIL_LARGE_FRAME_POOL_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(FAIO_DETAIL_LARGE_FRAME_POOL_ASAN)
#define FAIO_DETAIL_LARGE_FRAME_POOL_ASAN 1
#endif
#if defined(FAIO_DETAIL_LARGE_FRAME_POOL_ASAN)
#include <sanitizer/asan_interface.h>
#endif

namespace faio::detail {
/** @brief 分配准入仅检查原始 frame_bytes，不因 alignment 很大就将小帧视为大帧。 */
inline constexpr std::size_t large_frame_pool_min_frame_bytes = 1024 * 1024;

/**  @brief 普通匿名页源；不修改线程或进程的内存策略，不保证 NUMA 放置位置。 */
struct native_large_frame_page_source {
  /** @brief 返回实际基础页大小；后端不可用返回零并保存原因。 */
  static std::size_t page_size() noexcept {
#if defined(__unix__) || defined(__APPLE__)
    const auto result = ::sysconf(_SC_PAGESIZE);
    if (result > 0)
      return static_cast<std::size_t>(result);
#endif
    errno = static_cast<int>(std::errc::not_supported);
    return 0;
  }

  /** @brief 申请未预先访问页面的匿名映射；失败时返回空，由调用方回退到堆分配。 */
  static void* map(std::size_t bytes) noexcept {
#if defined(__unix__) || defined(__APPLE__)
    void* result =
        ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result != MAP_FAILED)
      return result;
#else
    (void)bytes;
    errno = static_cast<int>(std::errc::not_supported);
#endif
    return nullptr;
  }

  /** @brief 使用成功 map 时的原地址和完整跨度，不能根据对齐后的地址推测映射基址。 */
  static void unmap(void* mapping, std::size_t bytes) noexcept {
#if defined(__unix__) || defined(__APPLE__)
    (void)::munmap(mapping, bytes);
#else
    (void)mapping;
    (void)bytes;
#endif
  }

  /** @brief 只在页源失败后立即调用，保留真实系统错误码。 */
  static int last_error() noexcept { return errno; }
};

/** @brief 页源保持具体静态类型；测试可用同一协议注入映射失败，无运行期回调表。 */
template <class Source>
concept large_frame_page_source = requires(std::size_t bytes, void* memory) {
  { Source::page_size() } noexcept -> std::same_as<std::size_t>;
  { Source::map(bytes) } noexcept -> std::same_as<void*>;
  { Source::unmap(memory, bytes) } noexcept -> std::same_as<void>;
  { Source::last_error() } noexcept -> std::same_as<int>;
};

/**  @brief 低频诊断快照；空闲映射限额不限制活动映射、后备分配或进程 RSS。 */
struct large_frame_pool_statistics {
  std::size_t idle_limit_bytes{};
  std::size_t idle_mapped_bytes{};
  std::size_t active_mapped_bytes{};
  std::size_t live_extents{};
  std::uint64_t map_attempts{};
  std::uint64_t successful_maps{};
  std::uint64_t unmaps{};
  std::uint64_t reuse_hits{};
  std::uint64_t publications{};
  std::uint64_t late_returns{};
  std::uint64_t fallback_requests{};
  int last_backend_error{};
  bool retiring{};
};

/** @brief 大型协程帧分配池。 */
template <large_frame_page_source page_source_type>
class basic_large_frame_pool;
/** @brief 大型帧分配池的共享状态。 */
template <large_frame_page_source page_source_type>
struct large_frame_pool_state;

/**  @brief 活动期间由单一所有者持有的具体 extent，不借用 runtime、cache 或 TLS 的地址。 */
template <large_frame_page_source page_source_type>
class basic_large_frame_extent {
 public:
  /** @brief 禁止复制构造大型协程帧内存区。 */
  basic_large_frame_extent(const basic_large_frame_extent&) = delete;
  /** @brief 禁止复制赋值。 */
  basic_large_frame_extent& operator=(const basic_large_frame_extent&) = delete;

  /** @brief 本次分配的帧正文地址；调用者在地址前方的 prefix 区域建立自己的分配头。 */
  void* frame_address() const noexcept { return frame_; }

  /** @brief 完整匿名映射原地址，ASan 边界和最终 unmap 使用此值。 */
  void* mapping_address() const noexcept { return mapping_; }

  /** @brief 实际页取整跨度；所有池预算必须使用此值。 */
  std::size_t mapping_bytes() const noexcept { return mapping_bytes_; }

  /** @brief 复用键中的完整申请跨度：frame + prefix + 最坏对齐填充。 */
  std::size_t request_bytes() const noexcept { return request_bytes_; }

  /** @brief 编译器本次申请的原始字节数，不包含分配器前缀。 */
  std::size_t frame_bytes() const noexcept { return frame_bytes_; }

  /** @brief 保存的有效对齐至少满足 max_align_t。 */
  std::size_t alignment() const noexcept { return alignment_; }

 private:
  friend class basic_large_frame_pool<page_source_type>;
  friend struct large_frame_pool_state<page_source_type>;

  /** @brief 构造大型协程帧内存区。 */
  explicit basic_large_frame_extent(large_frame_pool_state<page_source_type>* owner) noexcept
      : owner_(owner) {}

  /** @brief 销毁大型协程帧内存区。 */
  ~basic_large_frame_extent() = default;

  large_frame_pool_state<page_source_type>* owner_;
  basic_large_frame_extent* next_{};
  void* mapping_{};
  void* frame_{};
  std::size_t mapping_bytes_{};
  std::size_t request_bytes_{};
  std::size_t frame_bytes_{};
  std::size_t alignment_{};
};

/** @brief 页内元数据也可全部标记为不可访问；空闲链只操作映射外的具体 extent 控制块。 */
inline void poison_large_frame_mapping([[maybe_unused]] void* memory,
                                       [[maybe_unused]] std::size_t bytes) noexcept {
#if defined(FAIO_DETAIL_LARGE_FRAME_POOL_ASAN)
  __asan_poison_memory_region(memory, bytes);
#endif
}

/** @brief acquire 先解除整个映射的不可访问标记，供调用方重建分配头；调用方随后恢复精确的 header、redzone 和 payload 边界。 */
inline void unpoison_large_frame_mapping([[maybe_unused]] void* memory,
                                         [[maybe_unused]] std::size_t bytes) noexcept {
#if defined(FAIO_DETAIL_LARGE_FRAME_POOL_ASAN)
  __asan_unpoison_memory_region(memory, bytes);
#endif
}

/**  @brief 地址稳定的接收端，由 runtime anchor、各 extent 的引用和临时 publisher guard 共同持有。 */
template <large_frame_page_source page_source_type>
struct large_frame_pool_state {
  using extent = basic_large_frame_extent<page_source_type>;

  /** @brief 构造大型帧分配池的共享状态。 */
  explicit large_frame_pool_state(std::size_t limit) noexcept : idle_limit(limit) {}

  /** @brief 禁止复制构造大型帧分配池的共享状态。 */
  large_frame_pool_state(const large_frame_pool_state&) = delete;
  /** @brief 禁止复制赋值。 */
  large_frame_pool_state& operator=(const large_frame_pool_state&) = delete;

  /** @brief 销毁大型帧分配池的共享状态。 */
  ~large_frame_pool_state() { assert(idle == nullptr && active_bytes == 0); }

  /** @brief 增加共享生命周期引用。 */
  void add_reference() noexcept { references.fetch_add(1, std::memory_order_relaxed); }

  /** @brief 归还共享生命周期引用。 */
  void release_reference() noexcept {
    if (references.fetch_sub(1, std::memory_order_acq_rel) == 1)
      delete this;
  }

  /**  @brief 独立 guard 不随节点发布而转移；析构时不再访问其他 state 字段。 */
  struct guard {
    /** @brief 持有页池共享状态的生命周期引用。 */
    explicit guard(large_frame_pool_state* state) noexcept : state_(state) {
      state_->add_reference();
    }

    /** @brief 禁止复制页池共享状态的生命周期守卫。 */
    guard(const guard&) = delete;
    /** @brief 禁止复制赋值。 */
    guard& operator=(const guard&) = delete;

    /** @brief 释放页池共享状态的生命周期引用。 */
    ~guard() { state_->release_reference(); }

    large_frame_pool_state* state_;
  };

  /**  @brief 查找并取走键完全匹配的空闲映射；映射外的元数据在 mutex 保护下发布。 */
  extent* acquire_idle(std::size_t request, std::size_t alignment) noexcept {
    std::lock_guard lock{mutex};
    if (retiring)
      return nullptr;
    auto** entry = &idle;
    while (*entry) {
      auto* current = *entry;
      if (current->request_bytes_ == request && current->alignment_ == alignment) {
        *entry = current->next_;
        current->next_ = nullptr;
        idle_bytes -= current->mapping_bytes_;
        active_bytes += current->mapping_bytes_;
        ++reuse_hits;
        return current;
      }
      entry = &current->next_;
    }
    return nullptr;
  }

  /**  @brief 映射慢路径不持有接收锁；extent 自身的引用保证 map 与池关闭交错发生时状态仍然有效。 */
  extent* map_new(std::size_t mapped,
                  std::size_t request,
                  std::size_t alignment,
                  int& failure) noexcept {
    auto* current = new (std::nothrow) extent(this);
    if (!current) {
      failure = static_cast<int>(std::errc::not_enough_memory);
      return nullptr;
    }
    add_reference();
    bool closed{};
    {
      std::lock_guard lock{mutex};
      closed = retiring;
      if (!closed)
        ++map_attempts;
    }
    if (closed) {
      delete current;
      release_reference();
      return nullptr;
    }
    current->mapping_ = page_source_type::map(mapped);
    if (!current->mapping_) {
      failure = page_source_type::last_error();
      if (!failure)
        failure = static_cast<int>(std::errc::not_enough_memory);
      delete current;
      release_reference();
      return nullptr;
    }
    current->mapping_bytes_ = mapped;
    current->request_bytes_ = request;
    current->alignment_ = alignment;
    bool accepted{};
    {
      std::lock_guard lock{mutex};
      ++successful_maps;
      if (!retiring) {
        active_bytes += mapped;
        ++live_extents;
        accepted = true;
      } else {
        ++unmaps;
      }
    }
    if (!accepted) {
      page_source_type::unmap(current->mapping_, mapped);
      delete current;
      release_reference();
      return nullptr;
    }
    return current;
  }

  /**  @brief 协程完整销毁后归还唯一租约，不保留运行时或缓存的任何借用地址。 */
  static void publish(extent* current) noexcept {
    auto* home = current->owner_;
    guard publisher{home};
    const auto bytes = current->mapping_bytes_;
    poison_large_frame_mapping(current->mapping_, bytes);
    if constexpr (requires {
                    { page_source_type::before_publish_lock() } noexcept -> std::same_as<void>;
                  })
      page_source_type::before_publish_lock();
    bool cached{};
    {
      std::lock_guard lock{home->mutex};
      assert(home->active_bytes >= bytes);
      home->active_bytes -= bytes;
      ++home->publications;
      if (!home->retiring && bytes <= home->idle_limit - home->idle_bytes) {
        current->next_ = home->idle;
        home->idle = current;
        home->idle_bytes += bytes;
        cached = true;
      } else {
        if (home->retiring)
          ++home->late_returns;
        --home->live_extents;
        ++home->unmaps;
      }
    }
    if constexpr (requires {
                    { page_source_type::after_publish_unlock() } noexcept -> std::same_as<void>;
                  })
      page_source_type::after_publish_unlock();
    if (!cached) {
      unpoison_large_frame_mapping(current->mapping_, bytes);
      page_source_type::unmap(current->mapping_, bytes);
      delete current;
      home->release_reference();
    }
  }

  /**  @brief trim 可重复调用；retire 永久关闭当前池实例，此后归还的映射不再进入空闲池。 */
  void detach_idle(bool close) noexcept {
    extent* detached{};
    {
      std::lock_guard lock{mutex};
      if (close)
        retiring = true;
      detached = std::exchange(idle, nullptr);
      idle_bytes = 0;
      for (auto* current = detached; current; current = current->next_) {
        --live_extents;
        ++unmaps;
      }
    }
    while (detached) {
      auto* current = detached;
      detached = current->next_;
      unpoison_large_frame_mapping(current->mapping_, current->mapping_bytes_);
      page_source_type::unmap(current->mapping_, current->mapping_bytes_);
      delete current;
      release_reference();
    }
  }

  /** @brief 读取当前状态快照。 */
  large_frame_pool_statistics snapshot() const noexcept {
    std::lock_guard lock{mutex};
    return {.idle_limit_bytes = idle_limit,
            .idle_mapped_bytes = idle_bytes,
            .active_mapped_bytes = active_bytes,
            .live_extents = live_extents,
            .map_attempts = map_attempts,
            .successful_maps = successful_maps,
            .unmaps = unmaps,
            .reuse_hits = reuse_hits,
            .publications = publications,
            .late_returns = late_returns,
            .retiring = retiring};
  }

  std::atomic<std::size_t> references{1}; /**< 初始引用属于仍存活的 pool 对象 anchor。 */
  mutable std::mutex mutex;
  extent* idle{};
  const std::size_t idle_limit;
  std::size_t idle_bytes{};
  std::size_t active_bytes{};
  std::size_t live_extents{};
  std::uint64_t map_attempts{};
  std::uint64_t successful_maps{};
  std::uint64_t unmaps{};
  std::uint64_t reuse_hits{};
  std::uint64_t publications{};
  std::uint64_t late_returns{};
  bool retiring{};
};

/**  @brief 运行时持有的可选页池；默认预算为零，表示关闭，仅在接纳分配请求时按需创建地址稳定的 state。 */
template <large_frame_page_source page_source_type = native_large_frame_page_source>
class basic_large_frame_pool {
 public:
  using extent = basic_large_frame_extent<page_source_type>;
  using state = large_frame_pool_state<page_source_type>;
  static constexpr auto minimum_frame_bytes = large_frame_pool_min_frame_bytes;

  /** @brief 构造大型协程帧分配池。 */
  explicit basic_large_frame_pool(std::size_t idle_limit_bytes = 0) noexcept
      : idle_limit_(idle_limit_bytes) {}

  /** @brief 禁止复制构造大型协程帧分配池。 */
  basic_large_frame_pool(const basic_large_frame_pool&) = delete;
  /** @brief 禁止复制赋值。 */
  basic_large_frame_pool& operator=(const basic_large_frame_pool&) = delete;

  /** @brief 销毁大型协程帧分配池。 */
  ~basic_large_frame_pool() {
    retire();
    if (auto* current = state_.exchange(nullptr, std::memory_order_acq_rel))
      current->release_reference();
  }

  /**
   *  @brief 分配准入接口；失败返回空，由调用方沿用原有堆分配协议；无效请求抛出 bad_alloc。
   * @details prefix 必须为 alignof(max_align_t) 的倍数，调用方在返回的 frame 前
   *          建立自己的 header；prefix 不满足对齐要求时视为无效请求，不自动增加映射填充。
   *          返回后整映射可写，调用方必须恢复原有精确 ASan redzone/payload 边界。
   */
  extent* try_acquire(std::size_t frame_bytes, std::size_t prefix_bytes, std::size_t alignment) {
    const auto request = checked_request(frame_bytes, prefix_bytes, alignment);
    if (frame_bytes < minimum_frame_bytes || idle_limit_ == 0)
      return nullptr;
    return acquire_checked(frame_bytes, prefix_bytes, request.first, request.second);
  }

  /**  @brief 唯一所有者在完整销毁协程后归还映射；pool 对象此时可以已被销毁。 */
  static void release(extent* current) noexcept {
    if (current)
      state::publish(current);
  }

  /**  @brief 清理当前空闲映射，不影响活动租约，后续仍允许复用。 */
  void trim() noexcept {
    if (auto* current = state_.load(std::memory_order_acquire))
      current->detach_idle(false);
  }

  /**  @brief 永久关闭池，现有活动租约仍可在之后安全归还，不重新启用旧池实例。 */
  void retire() noexcept {
    retired_.store(true, std::memory_order_seq_cst);
    if (auto* current = state_.load(std::memory_order_seq_cst))
      current->detach_idle(true);
  }

  /** @brief 取得低频诊断信息；尚未创建 state 时，不会仅为查询而初始化池。 */
  large_frame_pool_statistics statistics() const noexcept {
    auto* current = state_.load(std::memory_order_acquire);
    auto result = current ? current->snapshot()
                          : large_frame_pool_statistics{.idle_limit_bytes = idle_limit_};
    result.fallback_requests = fallback_requests_.load(std::memory_order_relaxed);
    result.last_backend_error = last_backend_error_.load(std::memory_order_relaxed);
    result.retiring = retired_.load(std::memory_order_acquire);
    return result;
  }

 private:
  /**  @brief 先校验对齐是否为有效的二次幂，并检查每次加法；不得通过后备路径掩盖请求溢出。 */
  static std::pair<std::size_t, std::size_t> checked_request(std::size_t frame,
                                                             std::size_t prefix,
                                                             std::size_t alignment) {
    if (!std::has_single_bit(alignment) || prefix % alignof(std::max_align_t) != 0)
      throw std::bad_alloc{};
    alignment = alignment < alignof(std::max_align_t) ? alignof(std::max_align_t) : alignment;
    const auto padding = alignment > alignof(std::max_align_t) ? alignment - 1 : 0;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (prefix > maximum - padding || frame > maximum - prefix - padding)
      throw std::bad_alloc{};
    return {frame + prefix + padding, alignment};
  }

  /** @brief 借用共享状态。 */
  state* get_state() noexcept {
    auto* current = state_.load(std::memory_order_acquire);
    if (!current) {
      auto candidate = std::unique_ptr<state>{new (std::nothrow) state(idle_limit_)};
      if (!candidate) {
        record_fallback(static_cast<int>(std::errc::not_enough_memory));
        return nullptr;
      }
      current = nullptr;
      if constexpr (requires {
                      { page_source_type::before_state_publish() } noexcept -> std::same_as<void>;
                    })
        page_source_type::before_state_publish();
      if (state_.compare_exchange_strong(
              current, candidate.get(), std::memory_order_seq_cst, std::memory_order_seq_cst))
        current = candidate.release();
      if constexpr (requires {
                      { page_source_type::after_state_publish() } noexcept -> std::same_as<void>;
                    })
        page_source_type::after_state_publish();
    }
    if (retired_.load(std::memory_order_seq_cst))
      current->detach_idle(true);
    return current;
  }

  /** @brief 在请求已校验后取得并对齐大帧内存区。 */
  extent* acquire_checked(std::size_t frame,
                          std::size_t prefix,
                          std::size_t request,
                          std::size_t alignment) {
    if (retired_.load(std::memory_order_acquire))
      return nullptr;
    const auto page = page_source_type::page_size();
    if (page == 0) {
      record_fallback(page_source_type::last_error());
      return nullptr;
    }
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (request > maximum - (page - 1))
      throw std::bad_alloc{};
    const auto mapped = (request + page - 1) / page * page;
    auto* home = get_state();
    if (!home)
      return nullptr;
    auto* current = home->acquire_idle(request, alignment);
    if (!current) {
      int failure{};
      current = home->map_new(mapped, request, alignment, failure);
      if (!current) {
        if (failure)
          record_fallback(failure);
        return nullptr;
      }
    }
    void* address = static_cast<std::byte*>(current->mapping_) + prefix;
    auto available = current->mapping_bytes_ - prefix;
    const auto aligned = std::align(alignment, frame, address, available);
    assert(aligned != nullptr);
    (void)aligned;
    current->frame_ = address;
    current->frame_bytes_ = frame;
    unpoison_large_frame_mapping(current->mapping_, current->mapping_bytes_);
    return current;
  }

  /** @brief 累计记录一次后备路径调用。 */
  void record_fallback(int error) noexcept {
    fallback_requests_.fetch_add(1, std::memory_order_relaxed);
    last_backend_error_.store(error, std::memory_order_relaxed);
  }

  const std::size_t idle_limit_;
  std::atomic<state*> state_{};
  std::atomic<bool> retired_{};
  std::atomic<std::uint64_t> fallback_requests_{};
  std::atomic<int> last_backend_error_{};
};

/** @brief 实际分配使用具体 native 页源别名，header 保存对应类型的 extent 指针。 */
using large_frame_pool = basic_large_frame_pool<>;
using large_frame_extent = large_frame_pool::extent;
}  // namespace faio::detail
#endif
