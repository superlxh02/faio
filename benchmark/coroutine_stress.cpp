#include "faio/faio.hpp"
#include "fastlog/fastlog.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace {

struct CoroutineStressConfig {
  std::size_t workers = 10000;
  std::size_t iterations_per_worker = 10000;
};

auto worker_task(std::size_t iterations,
                 std::atomic<std::uint64_t> &counter,
                 faio::sync::channel<int>::Sender done_sender)
    -> faio::task<void> {
  for (std::size_t i = 0; i < iterations; ++i) {
    counter.fetch_add(1, std::memory_order_relaxed);
    if ((i & 255U) == 0U) {
      co_await faio::time::sleep(std::chrono::microseconds(0));
    }
  }

  auto send_res = co_await done_sender.send(1);
  if (!send_res) {
    fastlog::console.error("done channel closed unexpectedly: {}",
                           send_res.error().message());
  }
  co_return;
}

auto run_stress(const CoroutineStressConfig config) -> faio::task<int> {
  auto [done_sender, done_receiver] = faio::sync::channel<int>::make(config.workers);
  std::atomic<std::uint64_t> counter{0};

  for (std::size_t i = 0; i < config.workers; ++i) {
    faio::spawn(worker_task(config.iterations_per_worker, counter, done_sender));
  }

  for (std::size_t i = 0; i < config.workers; ++i) {
    auto recv_res = co_await done_receiver.recv();
    if (!recv_res) {
      fastlog::console.error("recv done signal failed: {}", recv_res.error().message());
      co_return 1;
    }
  }

  const auto expected = config.workers * config.iterations_per_worker;
  const auto actual = counter.load(std::memory_order_relaxed);
  if (actual != expected) {
    fastlog::console.error("counter mismatch: expected={}, actual={}", expected, actual);
    co_return 1;
  }

  fastlog::console.info("coroutine stress passed: workers={}, iterations={}, total_ops={}",
                        config.workers, config.iterations_per_worker, actual);
  co_return 0;
}

} // namespace

int main(int argc, char **argv) {
  fastlog::set_consolelog_level(fastlog::LogLevel::Info);

  CoroutineStressConfig config;
  if (argc > 1) {
    config.workers = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
  }
  if (argc > 2) {
    config.iterations_per_worker =
        static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
  }

  faio::runtime_context ctx;

  const auto start = std::chrono::steady_clock::now();
  const int rc = faio::block_on(ctx, run_stress(config));
  const auto end = std::chrono::steady_clock::now();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  const double secs = static_cast<double>(ms) / 1000.0;
  const double total_ops =
      static_cast<double>(config.workers) * static_cast<double>(config.iterations_per_worker);
  const double throughput = secs > 0.0 ? (total_ops / secs) : 0.0;

  fastlog::console.info(
      "elapsed={}ms, throughput={:.2f} ops/s", ms, throughput);
  return rc;
}
