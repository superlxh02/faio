/**
 * @file benchmark/coro/faio_coro_benchmark.cpp
 * @brief 协程与同步原语性能基准。
 */
#include "faio/coroutine/composition.hpp"
#include "faio/detail/runtime/context.hpp"
#include "faio/detail/sync.hpp"
#include "faio/runtime/default.hpp"
#include <coroutine>
#include <cstddef>
#include <ratio>
#include <string>
#include <utility>
#include <variant>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;
/** @brief 仅在创建 worker 前设置，此后所有线程只读；批量模式不在每次操作时读取时钟。 */
bool sample_enabled = true;
bool warming = false;
std::filesystem::path sample_directory;

/** @brief 按采样模式读取单次操作的起始时刻。 */
clock_type::time_point sample_start() {
  return sample_enabled ? clock_type::now() : clock_type::time_point{};
}

/** @brief 计算单次操作的纳秒耗时，批量模式返回零。 */
double sample_elapsed(clock_type::time_point t) {
  return sample_enabled ? std::chrono::duration<double, std::nano>(clock_type::now() - t).count()
                        : 0.;
}

/** @brief 保存协程场景的摊销成本、延迟样本与迁移次数。 */
struct measurement {
  double ns_per_op{};
  std::vector<double> samples;
  std::size_t migrations{};
};

/** @brief 计算指定时刻以来的纳秒耗时。 */
double elapsed(clock_type::time_point start) {
  return std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
}

/** @brief 输出摊销成本、延迟分位数与样本数量。 */
void report(const std::string& name, measurement result) {
  if (warming)
    return;
  if (sample_enabled && !sample_directory.empty()) {
    std::ofstream out(sample_directory / (name + ".csv"));
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << std::setprecision(17) << "sample_index,latency_ns\n";
    for (std::size_t i = 0; i < result.samples.size(); ++i)
      out << i << ',' << result.samples[i] << '\n';
  }
  std::sort(result.samples.begin(), result.samples.end());
  auto p = [&](double q) {
    return result.samples[static_cast<std::size_t>((result.samples.size() - 1) * q)];
  };
  std::cout << name << ',' << std::fixed << std::setprecision(2) << result.ns_per_op << ',' << p(.5)
            << ',' << p(.9) << ',' << p(.99) << ',' << p(.999) << ',' << result.samples.back()
            << ',' << result.migrations << ',' << result.samples.size() << '\n';
}

/** @brief 为基准构造使用指定 worker 数量的运行时配置。 */
faio::runtime::Config config(std::size_t workers) {
  return faio::ConfigBuilder{}.set_num_workers(workers).build();
}

/** @brief 返回固定整数的惰性协程，用作测试负载。 */
faio::task<std::uint64_t> one() {
  co_return 1;
}

/** @brief 测量已就绪的同步原语、消息通道和任务等待操作。 */
faio::task<measurement> ready_paths(std::size_t n, int mode) {
  measurement r{0, std::vector<double>(n)};
  faio::sync::semaphore sem{1};
  faio::sync::mutex mutex;
  auto endpoints = faio::sync::mpsc<std::uint64_t>::make(64);
  auto& tx = endpoints.first;
  auto& rx = endpoints.second;
  std::uint64_t checksum = 0;
  auto start = clock_type::now();
  for (std::size_t i = 0; i < n; ++i) {
    auto t = sample_start();
    if (mode == 0) {
      co_await sem.acquire();
      sem.release();
    }
    if (mode == 1) {
      co_await mutex.lock();
      mutex.unlock();
    }
    if (mode == 2) {
      if (!(co_await tx.send(i)))
        throw std::runtime_error("send");
      auto v = co_await rx.recv();
      checksum += *v;
    }
    if (mode == 3) {
      if (!tx.try_send(i).value())
        throw std::runtime_error("try_send");
      checksum += rx.try_recv().value().value();
    }
    if (mode == 4)
      checksum += co_await one();
    r.samples[i] = sample_elapsed(t);
  }
  r.ns_per_op = elapsed(start) / n;
  if ((mode == 2 || mode == 3) && checksum != (n - 1) * n / 2)
    throw std::runtime_error("checksum");
  if (mode == 4 && checksum != n)
    throw std::runtime_error("checksum");
  co_return r;
}

/** @brief 测量协程主动让出及恢复的延迟与线程迁移。 */
faio::task<measurement> yields(std::size_t n) {
  measurement r{0, std::vector<double>(n)};
  auto start = clock_type::now();
  for (std::size_t i = 0; i < n; ++i) {
    const auto worker = co_await faio::this_coro::worker_id();
    auto t = sample_start();
    co_await faio::this_coro::yield();
    r.samples[i] = sample_elapsed(t);
    r.migrations += (co_await faio::this_coro::worker_id()) != worker;
  }
  r.ns_per_op = elapsed(start) / n;
  co_return r;
}

/** @brief 测量组合任务的等待与分支竞速。 */
faio::task<measurement> combinators(std::size_t n, int mode) {
  measurement r{0, std::vector<double>(n)};
  std::uint64_t sum = 0;
  auto start = clock_type::now();
  for (std::size_t i = 0; i < n; ++i) {
    auto t = sample_start();
    if (mode == 0) {
      auto h = faio::spawn(one());
      sum += co_await h;
    }
    if (mode == 1) {
      auto [a, b] = co_await faio::join(one(), one());
      sum += a + b;
    }
    if (mode == 2) {
      auto v = co_await faio::select(one(), one());
      sum += std::visit([](auto x) { return x; }, v.value);
    }
    r.samples[i] = sample_elapsed(t);
  }
  if (sum != n * (mode == 1 ? 2 : 1))
    throw std::runtime_error("checksum");
  r.ns_per_op = elapsed(start) / n;
  co_return r;
}

/** @brief 分组接口按每组 32 个 task 计时，n 是组数；不能与按消息计量的指标直接混合计算。 */
faio::task<measurement> task_groups(std::size_t n, bool scoped) {
  measurement r{0, std::vector<double>(n)};
  auto start = clock_type::now();
  for (auto& sample : r.samples) {
    auto t = sample_start();
    if (scoped) {
      co_await faio::scope([](faio::scope_context& group) -> faio::task<void> {
        for (int i = 0; i < 32; ++i)
          group.spawn(one());
        co_return;
      });
    } else {
      std::vector<faio::task<std::uint64_t>> tasks;
      tasks.reserve(32);
      for (int i = 0; i < 32; ++i)
        tasks.push_back(one());
      auto values = co_await faio::join_all(std::move(tasks));
      std::uint64_t sum = 0;
      for (auto value : values)
        sum += value;
      if (sum != 32)
        throw std::runtime_error("group checksum");
    }
    sample = sample_elapsed(t);
  }
  r.ns_per_op = elapsed(start) / n;
  co_return r;
}

/** @brief 测量单参与者屏障的立即完成路径。 */
faio::task<measurement> barrier_ready(std::size_t n) {
  faio::sync::barrier barrier{1};
  measurement r{0, std::vector<double>(n)};
  auto start = clock_type::now();
  for (auto& sample : r.samples) {
    auto t = sample_start();
    co_await barrier.arrive_and_wait();
    sample = sample_elapsed(t);
  }
  r.ns_per_op = elapsed(start) / n;
  co_return r;
}

/** @brief 记录提交到开始执行的耗时并递减剩余任务计数。 */
faio::task<void> record(clock_type::time_point sent,
                        double& output,
                        std::atomic<std::size_t>& left) {
  output = sample_elapsed(sent);
  if (left.fetch_sub(1, std::memory_order_acq_rel) == 1)
    left.notify_one();
  co_return;
}

/** @brief 在运行时的 worker 内批量提交延迟记录任务。 */
faio::task<void> internal_submit(measurement& r, std::atomic<std::size_t>& left) {
  for (auto& sample : r.samples)
    faio::spawn_detached(record(sample_start(), sample, left));
  co_return;
}

/** @brief 测量外部线程或 worker 批量提交任务的成本。 */
measurement burst(std::size_t n, std::size_t workers, bool internal) {
  faio::runtime::detail::runtime_context ctx{config(workers)};
  measurement r{0, std::vector<double>(n)};
  std::atomic<std::size_t> left{n};
  auto start = clock_type::now();
  if (internal)
    ctx.block_on(internal_submit(r, left));
  else {
    for (auto& sample : r.samples)
      ctx.submit(record(sample_start(), sample, left));
    for (auto seen = left.load(std::memory_order_acquire); seen;
         seen = left.load(std::memory_order_acquire))
      left.wait(seen);
  }
  r.ns_per_op = elapsed(start) / n;
  return r;
}

/**
 * @brief 两种实现的消息均为 16 字节：8 字节负载 + 8 字节时间戳；Rust Instant 本身可能为 16
 * 字节。
 */
struct message {
  std::uint64_t value;
  std::int64_t sent_ns;
};

static_assert(sizeof(message) == 16);

/** @brief 读取单调时钟的纳秒时间戳。 */
std::int64_t timestamp_ns() {
  return sample_enabled ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                              clock_type::now().time_since_epoch())
                              .count()
                        : 0;
}

using channel = faio::sync::mpsc<message>;

/** @brief 向有界通道发送带时间戳与序号的消息。 */
faio::task<void> producer(channel::sender tx, std::size_t n, std::size_t offset) {
  for (std::size_t i = 0; i < n; ++i)
    if (!(co_await tx.send({i + offset, timestamp_ns()})))
      throw std::runtime_error("closed");
}

/** @brief 测量多生产者消息通道吞吐与交付延迟。 */
faio::task<measurement> pipeline(std::size_t n, std::size_t producers, std::size_t capacity) {
  auto endpoints = channel::make(capacity);
  auto& tx = endpoints.first;
  auto& rx = endpoints.second;
  measurement r{0, std::vector<double>(n)};
  std::vector<faio::join_handle<void>> handles;
  handles.reserve(producers);
  const auto start = clock_type::now();
  for (std::size_t p = 0; p < producers; ++p)
    handles.push_back(faio::spawn(producer(tx, n / producers, p * (n / producers))));
  std::uint64_t sum = 0;
  for (auto& sample : r.samples) {
    auto m = co_await rx.recv();
    if (!m)
      throw std::runtime_error("closed");
    sample = static_cast<double>(timestamp_ns() - m->sent_ns);
    sum += m->value;
  }
  for (auto& h : handles)
    co_await h;
  if (sum != (n - 1) * n / 2)
    throw std::runtime_error("checksum");
  r.ns_per_op = elapsed(start) / n;
  co_return r;
}

/**
 * @brief 两个独立的单 worker 运行时保证两个协程在不同线程上恢复。
 * 测得的是两次信号量交接的 RTT；RTT/2 不能视为实测的单向延迟分位数。
 */
faio::task<void> echo(faio::sync::semaphore& request, faio::sync::semaphore& reply, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    co_await request.acquire();
    reply.release();
  }
}

/** @brief 通过请求与回复许可测量逐次往返延迟。 */
faio::task<measurement> ping(faio::sync::semaphore& request,
                             faio::sync::semaphore& reply,
                             std::size_t n) {
  measurement r{0, std::vector<double>(n)};
  auto start = clock_type::now();
  for (std::size_t i = 0; i < n; ++i) {
    auto t = sample_start();
    request.release();
    co_await reply.acquire();
    r.samples[i] = sample_elapsed(t);
  }
  r.ns_per_op = elapsed(start) / n;
  co_return r;
}

/** @brief 测量两个独立运行时之间的许可往返延迟。 */
measurement cross_runtime(std::size_t n) {
  faio::runtime::detail::runtime_context a{config(1)}, b{config(1)};
  faio::sync::semaphore request{0}, reply{0};
  auto h = b.spawn_observed(echo(request, reply, n));
  auto r = a.block_on(ping(request, reply, n));
  h.get();
  return r;
}

/** @brief 完成等待登记后才允许外部线程发送，避免 acquire 因已有许可而立即完成。 */
struct observed_acquire {
  faio::sync::semaphore::acquire_awaiter inner;
  std::atomic<std::size_t>& armed;
  std::size_t sequence;

  /** @brief 检查被观察的信号量获取是否立即完成。 */
  bool await_ready() { return inner.await_ready(); }

  /** @brief 登记获取等待并向通知线程发布就绪序号。 */
  bool await_suspend(std::coroutine_handle<> h) {
    auto* flag = &armed;
    const auto number = sequence;
    const auto suspended = inner.await_suspend(h);
    flag->store(number, std::memory_order_release);
    return suspended;
  }

  /** @brief 领取被观察的信号量获取结果。 */
  void await_resume() { inner.await_resume(); }
};

/** @brief 等待外部线程通知并记录恢复延迟。 */
faio::task<measurement> notification_receiver(faio::sync::semaphore& sem,
                                              std::atomic<std::size_t>& armed,
                                              std::atomic<std::size_t>& ack,
                                              const clock_type::time_point& sent,
                                              std::size_t n) {
  measurement r{0, std::vector<double>(n)};
  for (std::size_t i = 0; i < n; ++i) {
    co_await observed_acquire{sem.acquire(), armed, i + 1};
    r.samples[i] = sample_elapsed(sent);
    ack.store(i + 1, std::memory_order_release);
  }
  co_return r;
}

/** @brief 测量普通线程唤醒挂起协程的交付延迟。 */
measurement external_notification(std::size_t n) {
  faio::runtime::detail::runtime_context ctx{config(1)};
  faio::sync::semaphore sem{0};
  std::atomic<std::size_t> armed{0}, ack{0};
  clock_type::time_point sent;
  auto handle = ctx.spawn_observed(notification_receiver(sem, armed, ack, sent, n));
  auto start = clock_type::now();
  for (std::size_t i = 0; i < n; ++i) {
    while (armed.load(std::memory_order_acquire) <= i)
      std::atomic_signal_fence(std::memory_order_seq_cst);
    sent = sample_start();
    sem.release();
    while (ack.load(std::memory_order_acquire) <= i)
      std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  auto r = handle.get();
  r.ns_per_op = elapsed(start) / n;
  return r;
}

/** @brief 测量同步线程进入并等待一个协程的固定开销。 */
measurement block_entry(std::size_t n) {
  faio::runtime::detail::runtime_context ctx{config(1)};
  measurement r{0, std::vector<double>(n)};
  auto start = clock_type::now();
  for (auto& v : r.samples) {
    auto t = sample_start();
    if (ctx.block_on(one()) != 1)
      throw std::runtime_error("result");
    v = sample_elapsed(t);
  }
  r.ns_per_op = elapsed(start) / n;
  return r;
}

/** @brief 单个 worker 上的两个任务交替交接许可，确保测量包含两条协程之间的切换。 */
faio::task<measurement> same_thread_handoff(std::size_t n) {
  faio::sync::semaphore request{0}, reply{0};
  auto handle = faio::spawn(echo(request, reply, n));
  auto r = co_await ping(request, reply, n);
  co_await handle;
  co_return r;
}

/**
 * @brief 争用测试在持有锁或许可期间调用 yield，使四个等待者排队。
 * samples 是完整操作的延迟；ns_per_op 是整段墙钟时间/n，两者应分开报告。
 */
faio::task<void> contention_child(faio::sync::mutex& mutex,
                                  faio::sync::semaphore& sem,
                                  measurement& r,
                                  std::atomic<std::size_t>& done,
                                  std::size_t begin,
                                  std::size_t n,
                                  bool use_mutex) {
  for (std::size_t i = begin; i < begin + n; ++i) {
    auto t = sample_start();
    if (use_mutex)
      co_await mutex.lock();
    else
      co_await sem.acquire();
    co_await faio::this_coro::yield();
    if (use_mutex)
      mutex.unlock();
    else
      sem.release();
    done.fetch_add(1, std::memory_order_relaxed);
    r.samples[i] = sample_elapsed(t);
  }
}

/** @brief 测量四个任务竞争互斥锁或信号量的成本。 */
faio::task<measurement> contention(std::size_t n, bool use_mutex) {
  faio::sync::mutex mutex;
  faio::sync::semaphore sem{2};
  measurement r{0, std::vector<double>(n)};
  std::atomic<std::size_t> done{0};
  std::vector<faio::join_handle<void>> handles;
  handles.reserve(4);
  auto start = clock_type::now();
  for (std::size_t i = 0; i < 4; ++i)
    handles.push_back(
        faio::spawn(contention_child(mutex, sem, r, done, i * (n / 4), n / 4, use_mutex)));
  for (auto& h : handles)
    co_await h;
  r.ns_per_op = elapsed(start) / n;
  if (done.load() != n)
    throw std::runtime_error("contention count");
  co_return r;
}

/** @brief 反复参与屏障并记录本任务分配的操作延迟。 */
faio::task<void> barrier_child(faio::sync::barrier& barrier,
                               measurement& r,
                               std::size_t begin,
                               std::size_t n) {
  for (std::size_t i = begin; i < begin + n; ++i) {
    auto t = sample_start();
    co_await barrier.arrive_and_wait();
    r.samples[i] = sample_elapsed(t);
  }
}

/** @brief 测量四个任务重复通过屏障的同步开销。 */
faio::task<measurement> barrier_contended(std::size_t n) {
  faio::sync::barrier barrier{4};
  measurement r{0, std::vector<double>(n)};
  std::vector<faio::join_handle<void>> handles;
  handles.reserve(4);
  auto start = clock_type::now();
  for (std::size_t i = 0; i < 4; ++i)
    handles.push_back(faio::spawn(barrier_child(barrier, r, i * (n / 4), n / 4)));
  for (auto& h : handles) {
    co_await h;
  }
  r.ns_per_op = elapsed(start) / n;
  co_return r;
}

/** @brief 通过条件变量交替更新请求与回复状态。 */
faio::task<void> cv_echo(faio::sync::mutex& mutex,
                         faio::sync::condition_variable& cv,
                         int& turn,
                         std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    co_await mutex.lock();
    co_await cv.wait(mutex, [&] { return turn == 1; });
    turn = 0;
    mutex.unlock();
    cv.notify_one();
  }
}

/** @brief 测量互斥锁及条件变量组合的往返通知。 */
faio::task<measurement> cv_roundtrip(std::size_t n) {
  faio::sync::mutex mutex;
  faio::sync::condition_variable cv;
  int turn = 0;
  auto h = faio::spawn(cv_echo(mutex, cv, turn, n));
  measurement r{0, std::vector<double>(n)};
  auto start = clock_type::now();
  for (auto& v : r.samples) {
    auto t = sample_start();
    co_await mutex.lock();
    turn = 1;
    cv.notify_one();
    co_await cv.wait(mutex, [&] { return turn == 0; });
    mutex.unlock();
    v = sample_elapsed(t);
  }
  co_await h;
  r.ns_per_op = elapsed(start) / n;
  co_return r;
}

/** @brief 子任务完成时递减一次性倒计数。 */
faio::task<void> latch_child(faio::sync::latch& latch) {
  latch.count_down();
  co_return;
}

/** @brief 测量倒计数的就绪等待或任务组汇合。 */
faio::task<measurement> latch_groups(std::size_t n, bool ready) {
  measurement r{0, std::vector<double>(n)};
  auto start = clock_type::now();
  faio::sync::latch opened{0};
  for (auto& v : r.samples) {
    auto t = sample_start();
    if (ready)
      co_await opened.wait();
    else {
      faio::sync::latch latch{32};
      std::vector<faio::join_handle<void>> handles;
      handles.reserve(32);
      for (int i = 0; i < 32; ++i)
        handles.push_back(faio::spawn(latch_child(latch)));
      co_await latch.wait();
      for (auto& h : handles)
        co_await h;
    }
    v = sample_elapsed(t);
  }
  r.ns_per_op = elapsed(start) / n;
  co_return r;
}

/** @brief 测量读取单调时钟的基准开销。 */
measurement timer_calibration(std::size_t n) {
  measurement r{0, std::vector<double>(n)};
  auto start = clock_type::now();
  for (auto& v : r.samples) {
    auto t = sample_start();
    v = sample_elapsed(t);
  }
  r.ns_per_op = elapsed(start) / n;
  return r;
}
}

/** @brief 执行协程、同步原语和并发控制场景并输出 CSV。 */
int main(int argc, char** argv) {
  auto n = argc > 1 ? std::stoull(argv[1]) : 100000;
  n = (n / 4) * 4;
  if (n < 32)
    throw std::invalid_argument("count >= 32");
  sample_enabled = argc < 3 || std::string(argv[2]) != "batch";
  if (argc > 3) {
    sample_directory = argv[3];
    std::filesystem::create_directories(sample_directory);
  }
  std::cout << "scenario,ns_per_op,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,"
               "migrations,operations\n";
  const auto requested_n = n;
  for (bool warm : {true, false}) {
    warming = warm;
    n = warm ? std::min(requested_n, 1024ull) : requested_n;
    report("timer_calibration", timer_calibration(n));
    for (auto workers : {1uz, 4uz}) {
      faio::runtime::detail::runtime_context ctx{config(workers)};
      (void)ctx.block_on(yields(1000));
      report("yield_" + std::to_string(workers), ctx.block_on(yields(n)));
      report("handoff_rtt_w" + std::to_string(workers),
             ctx.block_on(same_thread_handoff(std::min(n, 10000ull))));
      report("mutex_contention_p4_w" + std::to_string(workers), ctx.block_on(contention(n, true)));
      report("semaphore_contention_k2_p4_w" + std::to_string(workers),
             ctx.block_on(contention(n, false)));
      report("barrier_p4_w" + std::to_string(workers), ctx.block_on(barrier_contended(n)));
      report("cv_roundtrip_w" + std::to_string(workers),
             ctx.block_on(cv_roundtrip(std::min(n, 10000ull))));
      report("latch_fanin_32_w" + std::to_string(workers),
             ctx.block_on(latch_groups(n / 32, false)));
      report("spawn_join_" + std::to_string(workers), ctx.block_on(combinators(n, 0)));
      if (workers == 1) {
        const char* names[] = {
            "semaphore_ready", "mutex_ready", "mpsc_ready_64", "mpsc_try_64", "task_await_ready"};
        for (int mode = 0; mode < 5; ++mode)
          report(names[mode], ctx.block_on(ready_paths(n, mode)));
        report("latch_ready", ctx.block_on(latch_groups(n, true)));
        report("barrier_ready_1", ctx.block_on(barrier_ready(n)));
        report("join_all_32", ctx.block_on(task_groups(n / 32, false)));
        report("scope_32", ctx.block_on(task_groups(n / 32, true)));
        report("join_ready_2", ctx.block_on(combinators(n, 1)));
        report("select_spawn_drain_2", ctx.block_on(combinators(n, 2)));
      }
      for (auto capacity : {64uz, 1024uz})
        for (auto producers : {1uz, 4uz}) {
          report("mpsc_p" + std::to_string(producers) + "_w" + std::to_string(workers) + "_c"
                     + std::to_string(capacity),
                 ctx.block_on(pipeline(n, producers, capacity)));
        }
      report("external_burst_" + std::to_string(workers), burst(n, workers, false));
      report("internal_burst_" + std::to_string(workers), burst(n, workers, true));
    }
    report("cross_runtime_rtt", cross_runtime(std::min(n, 10000ull)));
    report("external_notification_registered", external_notification(std::min(n, 10000ull)));
    report("block_on_entry", block_entry(std::min(n, 10000ull)));
  }
}
