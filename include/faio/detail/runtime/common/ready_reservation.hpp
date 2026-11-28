/**
 * @file faio/detail/runtime/common/ready_reservation.hpp
 * @brief 固定就绪槽的共享生命周期；不依赖调度队列或默认调度器的完整定义。
 */
#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace faio::runtime::detail {
/** @brief 有界多生产者就绪任务队列。 */
template <std::size_t default_capacity>
class mpsc_ready_queue;

/**
 * @brief 固定负载槽及其预留生命周期，由具体就绪队列拥有。
 * @details shared_ptr 从具体 storage 转换为基类指针时共享原控制块，仍按实际派生类型析构；
 *          不通过裸基类指针 delete，不需要虚函数或额外分配。
 */
struct ready_reservation_storage {
  /** @brief 恢复预留槽位的位图字。 */
  struct alignas(128) bitmap_word {
    std::atomic<std::uint64_t> bits{};
  };

  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(std::atomic<bool>::is_always_lock_free);

  /** @brief 空闲索引不要求 FIFO；按字分散竞争，不等待就绪队列的消费进度。 */
  class free_index_pool {
   public:
    /** @brief 构造空闲槽位索引池。 */
    explicit free_index_pool(std::size_t capacity)
        : count_((capacity + 63) / 64), words_(std::make_unique<bitmap_word[]>(count_)) {
      for (std::size_t i = 0; i < count_; ++i) {
        const auto available = capacity - i * 64;
        words_[i].bits.store(
            available >= 64 ? ~std::uint64_t{} : (std::uint64_t{1} << available) - 1,
            std::memory_order_relaxed);
      }
    }

    /** @brief 尝试领取一个空闲槽位索引。 */
    std::optional<std::size_t> try_pop() noexcept {
      static thread_local std::size_t next_word =
          next_seed_.fetch_add(1, std::memory_order_relaxed);
      const auto start = next_word & (count_ - 1);
      for (std::size_t offset = 0; offset < count_; ++offset) {
        const auto index = (start + offset) & (count_ - 1);
        auto& bitmap = words_[index].bits;
        auto bits = bitmap.load(std::memory_order_relaxed);
        if (bits) {
          const auto bit = std::countr_zero(bits);
          if (bitmap.compare_exchange_strong(
                  bits, bits & (bits - 1), std::memory_order_acq_rel, std::memory_order_relaxed)) {
            next_word = index;
            return index * 64 + bit;
          }
        }
      }
      return std::nullopt;
    }

    /** @brief 归还一个空闲槽位索引。 */
    void push(std::size_t index) noexcept {
      const auto mask = std::uint64_t{1} << (index & 63);
      const auto previous = words_[index / 64].bits.fetch_or(mask, std::memory_order_release);
      assert(!(previous & mask));
      (void)previous;
    }

   private:
    const std::size_t count_;
    std::unique_ptr<bitmap_word[]> words_;
    inline static std::atomic<std::size_t> next_seed_{};
  };

  enum class reservation_phase : unsigned char { RESERVED, QUEUED, CONSUMED };

  /** @brief 恢复容量预留记录。 */
  struct reservation_record {
    std::atomic<std::size_t> users{};
    std::atomic<reservation_phase> phase{reservation_phase::RESERVED};
    std::shared_ptr<ready_reservation_storage> keepalive;
  };

  /** @brief 构造恢复预留槽位存储。 */
  explicit ready_reservation_storage(std::size_t count)
      : capacity(count),
        handles(std::make_unique<std::coroutine_handle<>[]>(count)),
        reserved(std::make_unique<bool[]>(count)),
        reservations(std::make_unique<reservation_record[]>(count)),
        free_indices(count) {}

  /** @brief 归还就绪容量预留。 */
  void release_reservation(std::size_t index) noexcept {
    auto& record = reservations[index];
    if (record.users.fetch_sub(1, std::memory_order_acq_rel) != 1)
      return;
    assert(record.phase.load(std::memory_order_relaxed) != reservation_phase::QUEUED);
    auto owner = std::move(record.keepalive);
    handles[index] = {};
    reserved[index] = false;
    free_indices.push(index);
  }

  const std::size_t capacity;
  std::unique_ptr<std::coroutine_handle<>[]> handles;
  std::unique_ptr<bool[]> reserved;
  std::unique_ptr<reservation_record[]> reservations;
  free_index_pool free_indices;
};

/** @brief 预留固定负载槽；复制只增加该槽引用，不分配或扩大队列。 */
template <std::size_t default_capacity>
class ready_reservation {
 public:
  /** @brief 构造协程恢复容量预留。 */
  ready_reservation() noexcept = default;

  /** @brief 复制构造协程恢复容量预留。 */
  ready_reservation(const ready_reservation& other) noexcept
      : state_(other.state_), index_(other.index_) {
    if (state_)
      state_->reservations[index_].users.fetch_add(1, std::memory_order_relaxed);
  }

  /** @brief 移动构造协程恢复容量预留。 */
  ready_reservation(ready_reservation&& other) noexcept
      : state_(std::exchange(other.state_, nullptr)), index_(other.index_) {}

  /** @brief 复制赋值。 */
  ready_reservation& operator=(const ready_reservation& other) noexcept {
    ready_reservation copy{other};
    swap(copy);
    return *this;
  }

  /** @brief 移动赋值。 */
  ready_reservation& operator=(ready_reservation&& other) noexcept {
    ready_reservation moved{std::move(other)};
    swap(moved);
    return *this;
  }

  /** @brief 销毁协程恢复容量预留。 */
  ~ready_reservation() {
    if (state_)
      state_->release_reservation(index_);
  }

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return state_ != nullptr; }

  /** @brief 判断资源是否属于指定域。 */
  bool belongs_to(const mpsc_ready_queue<default_capacity>& queue) const noexcept;
  /** @brief 通过预留槽发布就绪任务。 */
  bool publish(std::coroutine_handle<> handle) const noexcept;
  /** @brief 向所属工作线程发布就绪任务。 */
  bool publish_local(std::coroutine_handle<> handle) const noexcept;

 private:
  friend class mpsc_ready_queue<default_capacity>;

  /** @brief 构造协程恢复容量预留。 */
  ready_reservation(ready_reservation_storage* state, std::size_t index) noexcept
      : state_(state), index_(index) {}

  /** @brief 交换两个对象的状态。 */
  void swap(ready_reservation& other) noexcept {
    std::swap(state_, other.state_);
    std::swap(index_, other.index_);
  }

  ready_reservation_storage* state_{};
  std::size_t index_{};
};
}  // namespace faio::runtime::detail
