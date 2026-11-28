/**
 * @file faio/detail/runtime/multi_thread/scheduler/mpsc_ready_queue.hpp
 * @brief 启动时分配、运行期容量固定的无锁 MPSC 不可窃取队列。
 */
#pragma once

#include "faio/detail/runtime/common/ready_reservation.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/scq_index_ring.hpp"
#include <atomic>
#include <bit>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace faio::runtime::detail {
/**
 * @brief 由 N 个负载槽、位图空闲索引池和保存已发布索引的 SCQ 环组成。
 * @details 运行期不分配、不扩容。生产者先取得 free 索引，写完句柄后才发布；
 *          消费者先清空句柄，再归还空闲位；尚未发布的预留槽不阻塞其他就绪项。
 *          ready SCQ 容量是 2N（物理 4N 个固定元数据项），持有 payload 索引的
 *          并发生产者最多 N 个，加上唯一消费者，满足 SCQ 的并发数限制。
 *          try_push 不可接纳时返回 false，句柄仍归调用者。
 */
template <std::size_t default_capacity = 256>
class mpsc_ready_queue {
  using reservation_phase = ready_reservation_storage::reservation_phase;
  static_assert(std::atomic<bool>::is_always_lock_free);

  /** @brief 有界就绪队列的槽位存储。 */
  struct storage : ready_reservation_storage {
    /** @brief 构造有界就绪队列的槽位存储。 */
    explicit storage(std::size_t count)
        : ready_reservation_storage(count), ready_indices(count * 2) {}

    /** @brief 发布就绪任务。 */
    void publish(std::size_t index, std::coroutine_handle<> handle, bool local = false) noexcept {
      handles[index] = handle;
      bool empty_hint = false;
      if (local && !lookahead
          && has_ready.compare_exchange_strong(
              empty_hint, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        lookahead = index;
        return;
      }
      ready_indices.push(index);
      has_ready.store(true, std::memory_order_release);
    }

    /** @brief 将就绪任务写入预留槽位。 */
    bool publish_reserved(std::size_t index,
                          std::coroutine_handle<> handle,
                          bool local = false) noexcept {
      if (!handle || !accepting.load(std::memory_order_acquire))
        return false;
      auto expected = reservation_phase::RESERVED;
      auto& record = reservations[index];
      if (!record.phase.compare_exchange_strong(expected,
                                                reservation_phase::QUEUED,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed))
        return false;
      record.users.fetch_add(1, std::memory_order_relaxed);
      publish(index, handle, local);
      return true;
    }

    /** @brief 尝试领取一个就绪任务。 */
    std::optional<std::coroutine_handle<>> try_pop() noexcept {
      if (!lookahead && !has_ready.load(std::memory_order_acquire))
        return std::nullopt;
      if (!lookahead)
        refill();
      if (!lookahead)
        return std::nullopt;
      const auto index = *std::exchange(lookahead, std::nullopt);
      const auto handle = std::exchange(handles[index], std::coroutine_handle<>{});
      if (reserved[index]) {
        reservations[index].phase.store(reservation_phase::CONSUMED, std::memory_order_release);
        release_reservation(index);
      } else {
        free_indices.push(index);
      }
      refill();
      return handle;
    }

    /** @brief 从就绪索引环补充预读索引，未取得索引时更新并复查就绪提示。 */
    void refill() noexcept {
      assert(!lookahead);
      lookahead = ready_indices.try_pop();
      if (lookahead)
        return;
      has_ready.exchange(false, std::memory_order_acq_rel);
      lookahead = ready_indices.try_pop();
      if (lookahead)
        has_ready.store(true, std::memory_order_release);
    }

    scq_index_ring<true> ready_indices;
    alignas(128) std::atomic<bool> accepting{true};
    alignas(128) std::atomic<bool> has_ready{};
    alignas(128) std::optional<std::size_t> lookahead;
  };

 public:
  using reservation = ready_reservation<default_capacity>;

  /** @brief 构造有界多生产者就绪任务队列。 */
  explicit mpsc_ready_queue(std::size_t capacity = default_capacity)
      : state_(std::make_shared<storage>(checked_capacity(capacity))) {}

  /** @brief 禁止复制构造有界多生产者就绪任务队列。 */
  mpsc_ready_queue(const mpsc_ready_queue&) = delete;
  /** @brief 禁止复制赋值。 */
  mpsc_ready_queue& operator=(const mpsc_ready_queue&) = delete;
  /** @brief 禁止移动构造有界多生产者就绪任务队列。 */
  mpsc_ready_queue(mpsc_ready_queue&&) = delete;
  /** @brief 禁止移动赋值。 */
  mpsc_ready_queue& operator=(mpsc_ready_queue&&) = delete;

  /** @brief 销毁有界多生产者就绪任务队列。 */
  ~mpsc_ready_queue() {
    state_->accepting.store(false, std::memory_order_release);
    while (state_->try_pop()) {
    }
  }

  /** @brief 预留后续恢复或提交所需容量。 */
  reservation reserve() noexcept {
    auto* state = state_.get();
    if (!state->accepting.load(std::memory_order_acquire))
      return {};
    const auto index = state->free_indices.try_pop();
    if (!index)
      return {};
    auto& record = state->reservations[*index];
    record.keepalive = state_;
    record.phase.store(reservation_phase::RESERVED, std::memory_order_relaxed);
    record.users.store(1, std::memory_order_relaxed);
    state->reserved[*index] = true;
    return reservation{state, *index};
  }

  /** @brief 尝试向有界队列发布一个就绪任务。 */
  bool try_push(std::coroutine_handle<> handle) noexcept {
    auto* state = state_.get();
    if (!handle || !state->accepting.load(std::memory_order_acquire))
      return false;
    const auto index = state->free_indices.try_pop();
    if (!index)
      return false;
    state->publish(*index, handle);
    return true;
  }

  /** @brief 尝试向所属线程发布一个就绪任务。 */
  bool try_push_local(std::coroutine_handle<> handle) noexcept {
    auto* state = state_.get();
    if (!handle || !state->accepting.load(std::memory_order_acquire))
      return false;
    const auto index = state->free_indices.try_pop();
    if (!index)
      return false;
    state->publish(*index, handle, true);
    return true;
  }

  /** @brief 向队列发布一个就绪任务。 */
  void push(std::coroutine_handle<> handle) {
    if (!handle)
      throw std::invalid_argument("不可窃取队列不能接收空协程句柄");
    if (!try_push(handle))
      throw std::overflow_error("不可窃取队列已满或暂不可接纳");
  }

  /** @brief 向所属线程队列发布一个就绪任务。 */
  void push_local(std::coroutine_handle<> handle) {
    if (!handle)
      throw std::invalid_argument("不可窃取队列不能接收空协程句柄");
    if (!try_push_local(handle))
      throw std::overflow_error("不可窃取队列已满或暂不可接纳");
  }

  /** @brief 尝试领取一个就绪任务。 */
  std::optional<std::coroutine_handle<>> try_pop() noexcept { return state_->try_pop(); }

  /** @brief 判断存储或队列是否为空。 */
  bool empty() const noexcept { return !state_->has_ready.load(std::memory_order_acquire); }

  /** @brief 读取固定存储容量。 */
  std::size_t capacity() const noexcept { return state_->capacity; }

  /** @brief 检查容量配置并返回有效值。 */
  static std::size_t checked_capacity(std::size_t capacity) {
    if (capacity < 2 || !std::has_single_bit(capacity) || capacity > (std::size_t{1} << 29))
      throw std::invalid_argument("不可窃取队列容量必须是 2..2^29 范围内的 2 的幂");
    return capacity;
  }

 private:
  friend struct mpsc_ready_queue_test_access;
  friend class ready_reservation<default_capacity>;
  std::shared_ptr<storage> state_;
};

/** @brief 发布任务与检查所属队列需要 storage 的完整定义；生命周期管理本身仅依赖轻量的基础头文件。 */
template <std::size_t default_capacity>
bool ready_reservation<default_capacity>::belongs_to(
    const mpsc_ready_queue<default_capacity>& queue) const noexcept {
  return state_ == queue.state_.get();
}

/** @brief 发布就绪任务。 */
template <std::size_t default_capacity>
bool ready_reservation<default_capacity>::publish(std::coroutine_handle<> handle) const noexcept {
  using storage = typename mpsc_ready_queue<default_capacity>::storage;
  return state_ && static_cast<storage*>(state_)->publish_reserved(index_, handle);
}

/** @brief 向所属工作线程发布就绪任务。 */
template <std::size_t default_capacity>
bool ready_reservation<default_capacity>::publish_local(
    std::coroutine_handle<> handle) const noexcept {
  using storage = typename mpsc_ready_queue<default_capacity>::storage;
  return state_ && static_cast<storage*>(state_)->publish_reserved(index_, handle, true);
}
}  // namespace faio::runtime::detail
