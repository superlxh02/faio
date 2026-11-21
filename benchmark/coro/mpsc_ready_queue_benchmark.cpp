/**
 * @file benchmark/coro/mpsc_ready_queue_benchmark.cpp
 * @brief 多生产者就绪队列性能基准。
 */
#include "faio/detail/runtime/multi_thread/scheduler/mpsc_ready_queue.hpp"
#include "faio/detail/common/util/cpu_relax.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <deque>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;
using handle_type = std::coroutine_handle<>;
using array_queue = faio::runtime::detail::mpsc_ready_queue<>;

/** @brief 与被测队列具有相同积压上限的对照实现。 */
class mutex_deque {
 public:
  /** @brief 设置对照队列的积压上限。 */
  explicit mutex_deque(std::size_t capacity) : capacity_(capacity) {}
  /** @brief 尝试向有界对照队列追加协程句柄。 */
  bool try_push(handle_type handle) {
    std::lock_guard lock(mutex_);
    if (tasks_.size() == capacity_) return false;
    tasks_.push_back(handle);
    return true;
  }
  /** @brief 尝试从对照队列按 FIFO 顺序取出协程句柄。 */
  std::optional<handle_type> try_pop() {
    std::lock_guard lock(mutex_);
    if (tasks_.empty()) return std::nullopt;
    const auto handle = tasks_.front();
    tasks_.pop_front();
    return handle;
  }

 private:
  const std::size_t capacity_;
  std::mutex mutex_;
  std::deque<handle_type> tasks_;
};

/** @brief 通过登记锁、队列锁与唤醒锁提交任务的基线实现。 */
class legacy_mailbox {
 public:
  /** @brief 设置对照队列的积压上限。 */
  explicit legacy_mailbox(std::size_t capacity) : capacity_(capacity) {}
  /** @brief 尝试向有界对照队列追加协程句柄。 */
  bool try_push(handle_type handle) {
    {
      std::lock_guard registration_lock(registration_mutex_);
      std::lock_guard queue_lock(queue_mutex_);
      if (tasks_.size() == capacity_) return false;
      const bool was_empty = tasks_.empty();
      tasks_.push_back(handle);
      if (was_empty) has_tasks_.store(true, std::memory_order_release);
    }
    std::lock_guard wake_lock(wake_mutex_);
    return true;
  }
  /** @brief 尝试从对照队列按 FIFO 顺序取出协程句柄。 */
  std::optional<handle_type> try_pop() {
    if (!has_tasks_.load(std::memory_order_acquire)) return std::nullopt;
    std::lock_guard lock(queue_mutex_);
    if (tasks_.empty()) return std::nullopt;
    const auto handle = tasks_.front();
    tasks_.pop_front();
    if (tasks_.empty()) has_tasks_.store(false, std::memory_order_release);
    return handle;
  }

 private:
  const std::size_t capacity_;
  std::mutex registration_mutex_;
  std::mutex queue_mutex_;
  std::mutex wake_mutex_;
  std::deque<handle_type> tasks_;
  std::atomic<bool> has_tasks_{};
};

/** @brief 监测队列内部停滞并在超时后结束基准进程。 */
class operation_watchdog {
 public:
  /** @brief 启动监测线程，在基准停滞时结束进程。 */
  operation_watchdog(std::string_view name, std::size_t producers)
      : observer_([done = finished_.get_future(), label = std::string{name}, producers] {
          if (done.wait_for(std::chrono::seconds{30}) == std::future_status::ready) return;
          std::fprintf(stderr, "queue benchmark timed out: %s, producers=%zu\n",
                       label.c_str(), producers);
          std::_Exit(2);
        }) {}
  /** @brief 禁止复制负责等待监测线程结束的对象。 */
  operation_watchdog(const operation_watchdog&) = delete;
  /** @brief 禁止复制负责等待监测线程结束的对象。 */
  operation_watchdog& operator=(const operation_watchdog&) = delete;
  /** @brief 通知监测线程基准操作已经结束。 */
  ~operation_watchdog() { finished_.set_value(); }

 private:
  std::promise<void> finished_;
  std::jthread observer_;
};

/** @brief 记录队列耗时与满队列、空队列重试次数。 */
struct measurement {
  double seconds{};
  std::size_t full_retries{};
  std::size_t empty_retries{};
};
/** @brief 对齐各生产者的统计计数，避免相互共享缓存行。 */
struct alignas(128) producer_result {
  std::size_t full_retries{};
};

/** @brief 在队列重试期间降低 CPU 忙等开销，并定期让出线程。 */
inline void retry_hint(std::size_t retries) noexcept {
  faio::util::cpu_relax();
  if (retries % 256 == 0) std::this_thread::yield();
}

/** @brief 测量同一线程入队及出队的成对开销。 */
template <class Queue>
measurement same_thread(std::size_t items, std::size_t capacity) {
  Queue queue{capacity};
  const handle_type handle = std::noop_coroutine();
  const auto begin = clock_type::now();
  for (std::size_t i = 0; i < items; ++i) {
    const bool accepted = [&] {
      if constexpr (requires { queue.try_push_local(handle); })
        return queue.try_push_local(handle);
      else
        return queue.try_push(handle);
    }();
    const auto selected = queue.try_pop();
    if (!accepted || !selected || selected->address() != handle.address())
      throw std::runtime_error("owner queue count/handle mismatch");
  }
  const auto elapsed = clock_type::now() - begin;
  if (queue.try_pop()) throw std::runtime_error("owner queue returned an extra handle");
  return {std::chrono::duration<double>{elapsed}.count(), 0, 0};
}

/** @brief 测量多生产者向单消费者提交任务的吞吐与重试次数。 */
template <class Queue>
measurement concurrent(std::size_t producer_count, std::size_t per_producer,
                       std::size_t capacity) {
  if (per_producer > std::numeric_limits<std::size_t>::max() / producer_count)
    throw std::runtime_error("message count overflows size_t");
  const auto total = producer_count * per_producer;
  Queue queue{capacity};
  std::atomic<bool> start{}, abort{};
  const handle_type expected_handle = std::noop_coroutine();
  std::vector<producer_result> results(producer_count);
  std::vector<std::jthread> producers;
  producers.reserve(producer_count);
  try {
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
      producers.emplace_back([&, producer] {
        while (!start.load(std::memory_order_acquire)) faio::util::cpu_relax();
        std::size_t retries{};
        for (std::size_t sent = 0; sent < per_producer;) {
          if (queue.try_push(expected_handle)) {
            ++sent;
          } else {
            if (abort.load(std::memory_order_relaxed)) break;
            retry_hint(++retries);
          }
        }
        results[producer].full_retries = retries;
      });
    }
  } catch (...) {
    abort.store(true, std::memory_order_relaxed);
    start.store(true, std::memory_order_release);
    throw;
  }

  std::size_t received{}, empty_retries{};
  bool bad_handle{};
  const auto begin = clock_type::now();
  const auto deadline = begin + std::chrono::seconds{30};
  start.store(true, std::memory_order_release);
  while (received < total) {
    if (const auto selected = queue.try_pop()) {
      if (selected->address() != expected_handle.address()) {
        bad_handle = true;
        break;
      }
      ++received;
    } else {
      retry_hint(++empty_retries);
      if (empty_retries % 256 == 0 && clock_type::now() >= deadline) break;
    }
  }
  const auto elapsed = clock_type::now() - begin;
  abort.store(true, std::memory_order_relaxed);
  for (auto& producer : producers) producer.join();
  if (bad_handle || received != total || queue.try_pop())
    throw std::runtime_error("concurrent queue count/handle mismatch or timeout");
  std::size_t full_retries{};
  for (const auto& result : results) full_retries += result.full_retries;
  return {std::chrono::duration<double>{elapsed}.count(), full_retries, empty_retries};
}

/** @brief 输出队列耗时分布、吞吐与重试统计。 */
template <class Queue>
void report(std::string_view name, std::size_t producers, std::size_t items,
            std::size_t rounds, std::size_t capacity) {
  std::vector<measurement> samples;
  samples.reserve(rounds);
  for (std::size_t round = 0; round < rounds; ++round) {
    operation_watchdog watchdog{name, producers};
    samples.push_back(producers == 0 ? same_thread<Queue>(items, capacity)
                                    : concurrent<Queue>(producers, items, capacity));
  }
  std::sort(samples.begin(), samples.end(), [](const auto& a, const auto& b) {
    return a.seconds < b.seconds;
  });
  const auto selected = samples[samples.size() / 2];
  const auto total = items * (producers == 0 ? 1 : producers);
  std::cout << std::left << std::setw(16) << name << std::setw(12)
            << (producers == 0 ? "owner" : "MPSC")
            << " producers=" << std::setw(2) << producers << " capacity=" << capacity
            << " messages=" << std::setw(9) << total << " median_ms=" << std::fixed
            << std::setprecision(3) << std::setw(10) << selected.seconds * 1'000
            << " Mmsg/s=" << total / selected.seconds / 1'000'000
            << " full_retries=" << selected.full_retries
            << " empty_retries=" << selected.empty_retries << '\n';
}

/** @brief 解析并校验大于零的基准参数。 */
std::size_t parse_positive(std::string_view input) {
  std::size_t value{};
  const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
  if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || value == 0)
    throw std::runtime_error("items, rounds and capacity must be positive integers");
  return value;
}
}

/** @brief 比较不同队列的单线程成本与多生产者吞吐。 */
int main(int argc, char** argv) {
  try {
    std::cout << std::unitbuf;
    if (argc > 4)
      throw std::runtime_error("usage: mpsc_ready_queue_benchmark [items-per-producer] [rounds] [capacity]");
    const auto items = argc > 1 ? parse_positive(argv[1]) : 100'000;
    const auto rounds = argc > 2 ? parse_positive(argv[2]) : 3;
    const auto capacity = argc > 3 ? parse_positive(argv[3]) : 256;
    if (capacity < 2 || !std::has_single_bit(capacity))
      throw std::runtime_error("capacity must be a power of two and at least 2");
    std::cout << "A message is one successful try_push plus one successful try_pop.\n"
              << "All implementations have the same fixed capacity; full/empty use the same retry policy.\n"
              << "No external per-message counter or coroutine allocation is timed.\n"
              << "legacy-mailbox models the replaced three-lock enqueue path with an awake owner.\n"
              << "logical CPUs=" << std::thread::hardware_concurrency()
              << "; producer counts above available CPUs are oversubscribed.\n";
    for (const auto producers : {0uz, 1uz, 4uz, 8uz, 16uz}) {
      report<mutex_deque>("bounded mutex", producers, items, rounds, capacity);
      report<legacy_mailbox>("legacy-mailbox", producers, items, rounds, capacity);
      report<array_queue>("array MPSC", producers, items, rounds, capacity);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
