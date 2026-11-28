/**
 * @file benchmark/io/blocking_pool_benchmark.cpp
 * @brief 对比固定与弹性用户阻塞池的生命周期、调度延迟和吞吐。
 */
#include "faio/detail/blocking/service.hpp"
#include "faio/detail/runtime/context.hpp"
#include "faio/runtime/default.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;
using pool_policy = faio::runtime::blocking_pool_policy;
using service = faio::blocking::service;

constexpr auto keep_alive = 50ms;
constexpr auto warm_gap = 2ms;
constexpr auto cold_gap = 90ms;
constexpr std::size_t lifecycle_samples = 80;
constexpr std::size_t sparse_samples = 250;
constexpr std::size_t burst_samples = 40;
constexpr std::size_t burst_size = 16;
constexpr auto burst_work_time = 1ms;
constexpr std::size_t throughput_batches = 60;
constexpr std::size_t throughput_batch_size = 1024;
constexpr std::size_t compute_batches = 30;
constexpr std::size_t compute_batch_size = 256;
constexpr std::size_t queue_limit = 4096;

double microseconds(clock_type::duration duration) {
  return std::chrono::duration<double, std::micro>(duration).count();
}

double percentile(std::span<const double> sorted, double fraction) {
  const auto rank = static_cast<std::size_t>(std::ceil(fraction * sorted.size()));
  return sorted[rank - 1];
}

std::uint64_t cpu_work(std::uint64_t value) {
  for (std::size_t i = 0; i < 20000; ++i) {
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    value *= 2685821657736338717ULL;
  }
  return value;
}

class benchmark {
 public:
  benchmark(pool_policy policy, std::size_t thread_limit)
      : policy_(policy), thread_limit_(thread_limit) {}

  void run() const {
    pool_lifecycle();
    runtime_lifecycle();
    warm_sparse();
    runtime_warm_sparse();
    cold_burst();
    throughput("warm_throughput", throughput_batches, throughput_batch_size, [](std::size_t) {});
    std::vector<std::uint64_t> results(compute_batch_size);
    throughput("cpu_throughput", compute_batches, compute_batch_size, [&results](std::size_t i) {
      results[i] = cpu_work(results[i] + i + 1);
    });
    if (std::ranges::find(results, 0) != results.end())
      throw std::logic_error("CPU workload checksum mismatch");
  }

 private:
  std::unique_ptr<service> make_pool() const {
    if (policy_ == pool_policy::ELASTIC)
      return std::make_unique<service>(thread_limit_, queue_limit, keep_alive);
    return std::make_unique<service>(thread_limit_, queue_limit);
  }

  faio::runtime::config make_runtime_config() const {
    return faio::config_builder{}
        .set_mode(faio::runtime::mode::CURRENT_THREAD)
        .set_max_blocking_threads(thread_limit_)
        .set_blocking_pool_policy(policy_)
        .set_blocking_keep_alive(keep_alive)
        .build();
  }

  template <std::invocable Job>
  static void submit(service& pool, Job&& job) {
    auto result = pool.try_submit(std::forward<Job>(job));
    if (!result)
      throw std::runtime_error(std::string{result.error().message()});
  }

  void report(std::string_view scenario,
              std::span<double> latency,
              std::size_t threads,
              double jobs_per_second = 0) const {
    std::ranges::sort(latency);
    std::cout << scenario << ',' << (policy_ == pool_policy::FIXED ? "fixed" : "elastic") << ','
              << thread_limit_ << ',' << latency.size() << ',';
    if (!latency.empty())
      std::cout << percentile(latency, 0.50) << ',' << percentile(latency, 0.99);
    else
      std::cout << ',';
    std::cout << ',' << jobs_per_second << ',' << threads << '\n';
  }

  void pool_lifecycle() const {
    std::vector<double> constructors(lifecycle_samples);
    std::vector<double> destructors(lifecycle_samples);
    std::size_t threads = 0;
    for (std::size_t i = 0; i <= lifecycle_samples; ++i) {
      const auto before = clock_type::now();
      auto pool = make_pool();
      const auto constructed = clock_type::now();
      threads = pool->started_threads();
      const auto closing = clock_type::now();
      pool.reset();
      const auto closed = clock_type::now();
      if (i != 0) {
        constructors[i - 1] = microseconds(constructed - before);
        destructors[i - 1] = microseconds(closed - closing);
      }
    }
    report("pool_construct", constructors, threads);
    report("pool_destroy", destructors, 0);
  }

  void runtime_lifecycle() const {
    const auto config = make_runtime_config();
    std::vector<double> constructors(lifecycle_samples);
    std::vector<double> destructors(lifecycle_samples);
    for (std::size_t i = 0; i <= lifecycle_samples; ++i) {
      const auto before = clock_type::now();
      auto runtime = std::make_unique<faio::runtime::detail::runtime_context>(config);
      const auto constructed = clock_type::now();
      runtime.reset();
      const auto closed = clock_type::now();
      if (i != 0) {
        constructors[i - 1] = microseconds(constructed - before);
        destructors[i - 1] = microseconds(closed - constructed);
      }
    }
    report("runtime_construct", constructors, policy_ == pool_policy::FIXED ? thread_limit_ : 0);
    report("runtime_destroy", destructors, 0);
  }

  static faio::task<std::size_t> run_sparse_tasks(std::span<double> latency) {
    co_await faio::spawn_blocking([] {});
    for (double& sample : latency) {
      std::this_thread::sleep_for(warm_gap);
      const auto queued = clock_type::now();
      sample = co_await faio::spawn_blocking(
          [queued] { return microseconds(clock_type::now() - queued); });
    }
    co_return faio::detail::current_blocking_pool()->service().started_threads();
  }

  void runtime_warm_sparse() const {
    std::vector<double> latency(sparse_samples);
    faio::runtime::detail::runtime_context runtime{make_runtime_config()};
    const auto threads = runtime.block_on(run_sparse_tasks(latency));
    report("spawn_blocking_warm_sparse_start", latency, threads);
  }

  void warm_sparse() const {
    std::vector<double> latency(sparse_samples);
    std::binary_semaphore done{0};
    auto pool = make_pool();
    submit(*pool, [&done] { done.release(); });
    done.acquire();
    for (double& sample : latency) {
      std::this_thread::sleep_for(warm_gap);
      const auto queued = clock_type::now();
      submit(*pool, [&sample, &done, queued] {
        sample = microseconds(clock_type::now() - queued);
        done.release();
      });
      done.acquire();
    }
    report("warm_sparse_start", latency, pool->started_threads());
  }

  void cold_burst() const {
    std::vector<double> latency(burst_samples * burst_size);
    std::vector<double> first_latency(burst_samples);
    std::atomic<std::size_t> pending{};
    std::binary_semaphore done{0};
    auto pool = make_pool();
    submit(*pool, [&done] { done.release(); });
    done.acquire();
    std::size_t before_threads = 0;
    std::size_t after_threads = 0;
    for (std::size_t i = 0; i < burst_samples; ++i) {
      std::this_thread::sleep_for(cold_gap);
      before_threads = std::max(before_threads, pool->started_threads());
      pending.store(burst_size, std::memory_order_relaxed);
      auto burst = std::span{latency}.subspan(i * burst_size, burst_size);
      for (double& sample : burst) {
        const auto queued = clock_type::now();
        submit(*pool, [&sample, &pending, &done, queued] {
          sample = microseconds(clock_type::now() - queued);
          std::this_thread::sleep_for(burst_work_time);
          if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
            done.release();
        });
      }
      after_threads = std::max(after_threads, pool->started_threads());
      done.acquire();
      first_latency[i] = burst.front();
    }
    report("cold_burst_first_start", first_latency, before_threads);
    report("cold_burst_all_starts", latency, after_threads);
  }

  template <typename Job>
    requires std::invocable<const Job&, std::size_t>
  void throughput(std::string_view scenario,
                  std::size_t batches,
                  std::size_t batch_size,
                  const Job& job) const {
    std::atomic<std::size_t> pending{};
    std::binary_semaphore done{0};
    auto pool = make_pool();
    const auto run_batch = [&] {
      pending.store(batch_size, std::memory_order_relaxed);
      for (std::size_t i = 0; i < batch_size; ++i) {
        submit(*pool, [&pending, &done, &job, i] {
          job(i);
          if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
            done.release();
        });
      }
      done.acquire();
    };
    run_batch();
    const auto start = clock_type::now();
    for (std::size_t i = 0; i < batches; ++i)
      run_batch();
    const auto elapsed = std::chrono::duration<double>(clock_type::now() - start).count();
    const auto jobs = static_cast<double>(batches * batch_size);
    report(scenario, {}, pool->started_threads(), jobs / elapsed);
  }

  pool_policy policy_;
  std::size_t thread_limit_;
};
}  // namespace

int main(int argc, char** argv) try {
  if (argc < 2 || argc > 3)
    throw std::invalid_argument("usage: faio_blocking_pool_benchmark fixed|elastic [thread_limit]");
  const std::string_view mode = argv[1];
  if (mode != "fixed" && mode != "elastic")
    throw std::invalid_argument("thread pool policy must be fixed or elastic");
  std::size_t limit = 64;
  if (argc == 3) {
    const std::string_view argument = argv[2];
    const auto end = argument.data() + argument.size();
    const auto [parsed, error] = std::from_chars(argument.data(), end, limit);
    if (error != std::errc{} || parsed != end || limit == 0)
      throw std::invalid_argument("thread_limit must be a positive integer");
  }
  const auto policy = mode == "fixed" ? pool_policy::FIXED : pool_policy::ELASTIC;
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "scenario,policy,thread_limit,samples,p50_us,p99_us,jobs_per_sec,blocking_threads\n";
  benchmark{policy, limit}.run();
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
