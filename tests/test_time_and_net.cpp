#include "backend_test_support.hpp"
#include <gtest/gtest.h>

#include "faio/faio.hpp"

#include <chrono>
#include <thread>
#include <vector>

TEST(TimeTest, SleepSuspendsAtLeastRequestedDuration) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().build()};
  auto t = []() -> faio::task<long long> {
    const auto start = std::chrono::steady_clock::now();
    co_await faio::time::sleep(std::chrono::milliseconds(10));
    const auto end = std::chrono::steady_clock::now();
    co_return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  };

  const auto elapsed_ms = ctx.block_on(t());
  EXPECT_GE(elapsed_ms, 8);
}

/** @brief 空时间轮闲置后注册的 deadline
 * 仍相对注册时刻，不能使用旧基准提前恢复。 */
TEST(TimeTest, IdleTimerKeepsNewSleepDeadlineInBothRuntimeModes) {
  for (auto mode : {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
    faio::runtime::detail::runtime_context runtime{
        faio_test::config_builder().set_mode(mode).set_num_workers(2).build()};
    // 原生线程等待令空轮积累 elapsed，而不注册任何定时任务。
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    auto measured_sleep = []() -> faio::task<long long> {
      const auto start = std::chrono::steady_clock::now();
      co_await faio::time::sleep(std::chrono::milliseconds{10});
      co_return std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
    };
    for (unsigned iteration = 0; iteration < 3; ++iteration)
      EXPECT_GE(runtime.block_on(measured_sleep()), 8);
  }
}

/** @brief 活跃高层轮保持 partial child 坐标；后插入短期限和连续 poll
 * 均不可提前。 */
TEST(TimeTest, ActiveHighLevelWheelPreservesDeadlinesAcrossIdleAndPartialPolls) {
  using clock = std::chrono::steady_clock;

  struct recording_sink {
    std::vector<clock::time_point> deliveries;

    void enqueue_ready(std::coroutine_handle<>) { deliveries.push_back(clock::now()); }
  } sink;

  faio::runtime::detail::timer::Timer timer{false};
  const auto long_deadline = clock::now() + std::chrono::milliseconds{220};
  timer.add_task(long_deadline, std::noop_coroutine());
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  const auto short_deadline = clock::now() + std::chrono::milliseconds{10};
  timer.add_task(short_deadline, std::noop_coroutine());
  timer.poll(sink);
  EXPECT_TRUE(sink.deliveries.empty());
  const auto limit = long_deadline + std::chrono::milliseconds{100};
  while (sink.deliveries.size() < 2 && clock::now() < limit) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    timer.poll(sink);
  }
  ASSERT_EQ(sink.deliveries.size(), 2u);
  EXPECT_GE(sink.deliveries[0], short_deadline);
  EXPECT_GE(sink.deliveries[1], long_deadline);
  EXPECT_TRUE(timer.empty());
}

TEST(NetAddressTest, ParseIpv4AndPort) {
  auto addr = faio::net::address::parse("127.0.0.1", 8080);
  ASSERT_TRUE(addr.has_value());
  EXPECT_TRUE(addr->is_ipv4());
  EXPECT_EQ(addr->port(), 8080);
}

TEST(NetAddressTest, ParseIpv6AndFormat) {
  auto addr = faio::net::address::parse("::1", 9000);
  ASSERT_TRUE(addr.has_value());
  EXPECT_TRUE(addr->is_ipv6());
  EXPECT_EQ(addr->port(), 9000);
  EXPECT_NE(addr->to_string().find("]:9000"), std::string::npos);
}

TEST(NetAddressTest, NumericParseRejectsHostname) {
  auto addr = faio::net::address::parse("localhost", 1234);
  EXPECT_FALSE(addr.has_value());
}
