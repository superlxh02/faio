/**
 * @file benchmark/coro/current_thread_benchmark.cpp
 * @brief 单线程运行时性能基准。
 */
#include "faio/detail/io/awaiter/recv.hpp"
#include "faio/detail/io/awaiter/send.hpp"
#include "faio/detail/runtime/context.hpp"
#include "faio/log/logger.hpp"
#include "faio/runtime/default.hpp"
#include "faio/time/timer.hpp"
#include <cstddef>
#include <ratio>
#include <string>
#include <utility>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;
using namespace std::chrono_literals;
using context = faio::runtime::detail::runtime_context;
bool sampling;

/** @brief 计算指定时刻以来的纳秒耗时。 */
double ns(clock_type::time_point start) {
  return std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
}

/** @brief 按采样模式读取单次操作的起始时刻。 */
clock_type::time_point stamp() {
  return sampling ? clock_type::now() : clock_type::time_point{};
}

/** @brief 保存当前线程基准的摊销成本与单次延迟样本。 */
struct result {
  /** @brief 为指定操作数量预分配延迟样本。 */
  explicit result(std::size_t n) : samples(n) {}

  double cost{};
  std::vector<double> samples;
};

/** @brief 输出摊销成本、延迟分位数与样本数量。 */
void report(std::string_view name, result r) {
  std::sort(r.samples.begin(), r.samples.end());
  const auto percentile = [&](double p) {
    return r.samples[static_cast<std::size_t>((r.samples.size() - 1) * p)];
  };
  std::cout << name << ',' << std::fixed << std::setprecision(2) << r.cost << ',' << percentile(.50)
            << ',' << percentile(.99) << ',' << percentile(.999) << ',' << r.samples.back() << ','
            << r.samples.size() << '\n';
}

/** @brief 返回固定整数的惰性协程，用作测试负载。 */
faio::task<int> one() {
  co_return 42;
}

/** @brief 测量让出执行权、直接等待和提交任务的单线程成本。 */
faio::task<result> coro(std::size_t n, int kind) {
  result r{n};
  const auto start = clock_type::now();
  for (auto& sample : r.samples) {
    const auto t = stamp();
    if (kind == 0)
      co_await faio::this_coro::yield();
    else if (kind == 1) {
      if (co_await one() != 42)
        throw std::runtime_error("task result");
    } else {
      auto h = faio::spawn(one());
      if (co_await h != 42)
        throw std::runtime_error("spawn result");
    }
    if (sampling)
      sample = ns(t);
  }
  r.cost = ns(start) / n;
  co_return r;
}

/** @brief 测量逐次提交并等待阻塞任务的成本。 */
faio::task<result> blocking_serial(std::size_t n) {
  result r{n};
  const auto start = clock_type::now();
  for (auto& sample : r.samples) {
    const auto t = stamp();
    auto h = faio::spawn_blocking([] { return 42; });
    if (co_await h != 42)
      throw std::runtime_error("blocking result");
    if (sampling)
      sample = ns(t);
  }
  r.cost = ns(start) / n;
  co_return r;
}

/** @brief 测量批量提交阻塞任务的耗时与完成延迟。 */
faio::task<result> blocking_burst(std::size_t n) {
  result r{n};
  std::vector<faio::join_handle<int>> handles;
  handles.reserve(n);
  const auto start = clock_type::now();
  for (std::size_t i = 0; i < n; ++i) {
    const auto t = stamp();
    handles.push_back(faio::spawn_blocking([&, i, t] {
      if (sampling)
        r.samples[i] = ns(t);
      return 42;
    }));
  }
  for (auto& h : handles)
    if (co_await h != 42)
      throw std::runtime_error("blocking burst result");
  r.cost = ns(start) / n;
  co_return r;
}

/** @brief 拥有本地往返测试所用的双向套接字对。 */
struct socket_pair {
  int fds[2];

  /** @brief 创建用于本地往返的套接字对。 */
  socket_pair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
      throw std::runtime_error("socketpair");
  }

  /** @brief 关闭往返负载拥有的两个本地套接字。 */
  ~socket_pair() {
    ::close(fds[0]);
    ::close(fds[1]);
  }

  /** @brief 禁止复制本地套接字对的所有权。 */
  socket_pair(const socket_pair&) = delete;

  /** @brief 禁止复制本地套接字对的所有权。 */
  socket_pair& operator=(const socket_pair&) = delete;
};

/** @brief 逐个读取并回写本地套接字上的单字节请求。 */
faio::task<void> echo(int fd, std::size_t n) {
  char byte;
  for (std::size_t i = 0; i < n; ++i) {
    if ((co_await faio::io::detail::Recv{fd, &byte, 1, 0}).value() != 1)
      throw std::runtime_error("echo receive");
    if ((co_await faio::io::detail::Send{fd, &byte, 1, MSG_NOSIGNAL}).value() != 1)
      throw std::runtime_error("echo send");
  }
}

/** @brief 测量本地套接字单字节请求的逐次往返延迟。 */
faio::task<result> io_roundtrip(std::size_t n) {
  socket_pair pair;
  auto peer = faio::spawn(echo(pair.fds[1], n));
  result r{n};
  char byte = 'x';
  const auto start = clock_type::now();
  for (auto& sample : r.samples) {
    const auto t = stamp();
    if ((co_await faio::io::detail::Send{pair.fds[0], &byte, 1, MSG_NOSIGNAL}).value() != 1)
      throw std::runtime_error("ping send");
    if ((co_await faio::io::detail::Recv{pair.fds[0], &byte, 1, 0}).value() != 1)
      throw std::runtime_error("ping receive");
    if (sampling)
      sample = ns(t);
  }
  co_await peer;
  r.cost = ns(start) / n;
  co_return r;
}

/** @brief 在定时基准运行期间持续让出协程执行权。 */
faio::task<void> busy(bool& done) {
  while (!done)
    co_await faio::this_coro::yield();
}

/** @brief 在持续就绪任务干扰下测量定时器完成延迟。 */
faio::task<result> timer_busy(std::size_t n) {
  bool done = false;
  auto worker = faio::spawn(busy(done));
  result r{n};
  const auto start = clock_type::now();
  for (auto& sample : r.samples) {
    const auto deadline = clock_type::now() + 1ms;
    co_await faio::time::sleep_until(deadline);
    if (sampling)
      sample = std::max(0., ns(deadline));
  }
  done = true;
  co_await worker;
  r.cost = ns(start) / n;
  co_return r;
}

/** @brief 等待并校验一组已提交任务的返回值。 */
faio::task<void> drain(std::vector<faio::join_handle<int>>& handles) {
  for (auto& h : handles)
    if (co_await h != 42)
      throw std::runtime_error("external result");
}

/** @brief 测量外部线程批量提交任务的完成延迟。 */
result external_burst(context& ctx, std::size_t n) {
  result r{n};
  std::vector<faio::join_handle<int>> handles;
  handles.reserve(n);
  const auto start = clock_type::now();
  for (std::size_t i = 0; i < n; ++i)
    handles.push_back(ctx.spawn_observed(one()));
  ctx.block_on(drain(handles));
  r.cost = ns(start) / n;
  return r;
}
}

/** @brief 执行当前线程运行时的协程、阻塞与 I/O 基准。 */
int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::stoull(argv[1]) : 20000;
  if (n < 32)
    throw std::invalid_argument("count must be >= 32");
  sampling = argc < 3 || std::string_view(argv[2]) != "batch";
  const bool multi = argc > 3 && std::string_view(argv[3]) == "multi";
  const auto config =
      faio::ConfigBuilder{}
          .set_mode(multi ? faio::runtime::mode::multi_thread : faio::runtime::mode::current_thread)
          .set_num_workers(1)
          .set_max_blocking_threads(4)
          .build();
  faio::log::logger()->set_level(spdlog::level::off);
  context ctx{config};
  std::cout << "scenario,ns_per_op,p50_ns,p99_ns,p999_ns,max_ns,operations\n";
  result calibration{n};
  const auto calibration_start = clock_type::now();
  for (auto& sample : calibration.samples) {
    const auto t = clock_type::now();
    sample = ns(t);
  }
  calibration.cost = ns(calibration_start) / n;
  report("clock_two_reads", std::move(calibration));
  report("blocking_cold", ctx.block_on(blocking_serial(1)));
  (void)ctx.block_on(coro(1000, 0));
  (void)ctx.block_on(blocking_serial(128));
  for (const auto kind : {0, 1, 2})
    report(kind == 0   ? "yield"
           : kind == 1 ? "task_await"
                       : "spawn_join",
           ctx.block_on(coro(n, kind)));
  result entry{100};
  auto start = clock_type::now();
  for (auto& sample : entry.samples) {
    const auto t = stamp();
    if (ctx.block_on(one()) != 42)
      throw std::runtime_error("entry result");
    if (sampling)
      sample = ns(t);
  }
  entry.cost = ns(start) / entry.samples.size();
  report("block_on_entry", std::move(entry));
  report("external_burst", external_burst(ctx, n));
  report("blocking_warm", ctx.block_on(blocking_serial(std::min(n, 2000uz))));
  report("blocking_burst_enqueue_to_start", ctx.block_on(blocking_burst(n)));
  report("socketpair_rtt", ctx.block_on(io_roundtrip(std::min(n, 2000uz))));
  report("timer_1ms_busy_lateness", ctx.block_on(timer_busy(32)));
}
