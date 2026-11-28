/**
 * @file faio/detail/runtime/multi_thread/scheduler/scq_index_ring.hpp
 * @brief 固定容量、无锁的 SCQ 索引环；不分配运行期节点。
 */
#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

namespace faio::runtime::detail {
/**
 * @brief ring 的逻辑容量是 N；2N 个元数据槽用于 SCQ 的周期与安全标记。
 * @details 调用 push 的线程必须已经拥有一个本环不包含的索引。
 *          外层固定负载槽池维持该约束，因此 push 不会等待 consumer 腾位。
 *          容量固定为启动时的 2 的幂，SCQ 用 FAA 领取候选位置并帮助越过旧周期。
 *          tail 也可能被 dequeue 的 catch-up 修改，必须保留原子 FAA/CAS 协议。
 */
template <bool single_consumer = false>
class scq_index_ring {
  using word = std::uint64_t;
  using signed_word = std::int64_t;
  static_assert(std::atomic<word>::is_always_lock_free);
  static_assert(std::atomic<signed_word>::is_always_lock_free);

 public:
  /** @brief 构造有界索引环形队列。 */
  explicit scq_index_ring(std::size_t capacity, bool initially_full = false)
      : capacity_(checked_capacity(capacity)),
        ring_size_(capacity_ * 2),
        ring_mask_(ring_size_ - 1),
        cycle_mask_(ring_size_ * 2 - 1),
        order_(std::countr_zero(capacity_)),
        entries_(std::make_unique<std::atomic<word>[]>(ring_size_)) {
    constexpr auto all_bits = ~word{};
    for (std::size_t i = 0; i < ring_size_; ++i)
      entries_[i].store(all_bits, std::memory_order_relaxed);
    if (initially_full) {
      for (std::size_t i = 0; i < capacity_; ++i)
        entries_[map(i)].store(ring_size_ + raw_map(i, order_, capacity_),
                               std::memory_order_relaxed);
      threshold_.store(max_threshold(), std::memory_order_relaxed);
      tail_.store(capacity_, std::memory_order_relaxed);
    }
  }

  /** @brief 禁止复制构造有界索引环形队列。 */
  scq_index_ring(const scq_index_ring&) = delete;
  /** @brief 禁止复制赋值。 */
  scq_index_ring& operator=(const scq_index_ring&) = delete;
  /** @brief 禁止移动构造有界索引环形队列。 */
  scq_index_ring(scq_index_ring&&) = delete;
  /** @brief 禁止移动赋值。 */
  scq_index_ring& operator=(scq_index_ring&&) = delete;
  /** @brief 销毁有界索引环形队列。 */
  ~scq_index_ring() = default;

  /** @brief 持有的索引成为本环元素；容量由外层索引守恒保证。 */
  void push(std::size_t index) noexcept {
    assert(index < capacity_);
    const auto encoded = static_cast<word>(index) ^ ring_mask_;
    for (;;) {
      const auto tail = tail_.fetch_add(1, std::memory_order_acq_rel);
      const auto cycle = (tail << 1) | cycle_mask_;
      auto& slot = entries_[map(tail)];
      auto entry = slot.load(std::memory_order_acquire);
      for (;;) {
        const auto entry_cycle = entry | cycle_mask_;
        if (!less(entry_cycle, cycle)
            || (entry != entry_cycle
                && (entry != (entry_cycle ^ ring_size_)
                    || greater(head_.load(std::memory_order_acquire), tail))))
          break;
        if (slot.compare_exchange_weak(
                entry, cycle ^ encoded, std::memory_order_acq_rel, std::memory_order_acquire)) {
          if (threshold_.load(std::memory_order_seq_cst) != max_threshold())
            threshold_.store(max_threshold(), std::memory_order_seq_cst);
          return;
        }
      }
    }
  }

  /** @brief 环为空或暂时没有可领取索引时返回空，不等待暂停的生产者。 */
  std::optional<std::size_t> try_pop() noexcept {
    if (threshold_.load(std::memory_order_seq_cst) < 0)
      return std::nullopt;
    for (;;) {
      const auto head = head_.fetch_add(1, std::memory_order_acq_rel);
      const auto cycle = (head << 1) | cycle_mask_;
      auto& slot = entries_[map(head)];
      auto entry = slot.load(std::memory_order_acquire);
      unsigned attempts{};
      for (;;) {
        const auto entry_cycle = entry | cycle_mask_;
        if (entry_cycle == cycle) {
          slot.fetch_or(ring_mask_, std::memory_order_acq_rel);
          return static_cast<std::size_t>(entry & ring_mask_);
        }
        word desired;
        if ((entry | ring_size_) != entry_cycle) {
          desired = entry & ~static_cast<word>(ring_size_);
          if (entry == desired)
            break;
        } else {
          if (++attempts <= 8) {
            entry = slot.load(std::memory_order_acquire);
            continue;
          }
          desired = cycle ^ ((~entry) & ring_size_);
        }
        if (!less(entry_cycle, cycle)
            || slot.compare_exchange_weak(
                entry, desired, std::memory_order_acq_rel, std::memory_order_acquire))
          break;
      }
      const auto tail = tail_.load(std::memory_order_acquire);
      if (!greater(tail, head + 1)) {
        catch_up(tail, head + 1);
        threshold_.fetch_sub(1, std::memory_order_acq_rel);
        return std::nullopt;
      }
      if (threshold_.fetch_sub(1, std::memory_order_acq_rel) <= 0)
        return std::nullopt;
    }
  }

  /** @brief 线程安全的只读提示；入队尚未完成时，队列也可能暂时报告为非空。 */
  bool empty() const noexcept { return threshold_.load(std::memory_order_seq_cst) < 0; }

  /** @brief 读取固定存储容量。 */
  std::size_t capacity() const noexcept { return capacity_; }

  /** @brief 检查容量配置并返回有效值。 */
  static std::size_t checked_capacity(std::size_t capacity) {
    if (capacity < 2 || !std::has_single_bit(capacity) || capacity > (std::size_t{1} << 30))
      throw std::invalid_argument("不可窃取队列容量必须是 2..2^30 范围内的 2 的幂");
    return capacity;
  }

 private:
  /** @brief 比较左值是否小于右值。 */
  static bool less(word left, word right) noexcept {
    return std::bit_cast<signed_word>(left - right) < 0;
  }

  /** @brief 比较左值是否大于右值。 */
  static bool greater(word left, word right) noexcept {
    return std::bit_cast<signed_word>(left - right) > 0;
  }

  /** @brief 按环大小和位数旋转索引，计算底层槽位位置。 */
  static std::size_t raw_map(word index, unsigned bits, std::size_t size) noexcept {
    const auto rotate = bits < 4 ? bits : 4;
    return static_cast<std::size_t>(((index & (size - 1)) >> (bits - rotate))
                                    | ((index << rotate) & (size - 1)));
  }

  /** @brief 将逻辑索引映射为当前环的槽位位置。 */
  std::size_t map(word index) const noexcept { return raw_map(index, order_ + 1, ring_size_); }

  /** @brief 返回根据环容量计算的最大重试阈值。 */
  signed_word max_threshold() const noexcept {
    return static_cast<signed_word>(capacity_ + ring_size_ - 1);
  }

  /** @brief 当 tail 落后于 head 时，尝试将 tail 推进到 head。 */
  void catch_up(word tail, word head) noexcept {
    while (!tail_.compare_exchange_weak(
        tail, head, std::memory_order_acq_rel, std::memory_order_acquire)) {
      head = head_.load(std::memory_order_acquire);
      tail = tail_.load(std::memory_order_acquire);
      if (!less(tail, head))
        return;
    }
  }

  const std::size_t capacity_;
  const std::size_t ring_size_;
  const std::size_t ring_mask_;
  const std::size_t cycle_mask_;
  const unsigned order_;
  std::unique_ptr<std::atomic<word>[]> entries_;
  alignas(128) std::atomic<word> head_{};
  alignas(128) std::atomic<signed_word> threshold_{-1};
  alignas(128) std::atomic<word> tail_{};
};
}  // namespace faio::runtime::detail
