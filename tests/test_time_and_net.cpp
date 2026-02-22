#include <gtest/gtest.h>

#include "faio/faio.hpp"

#include <chrono>

TEST(TimeTest, SleepSuspendsAtLeastRequestedDuration) {
  faio::runtime_context ctx;
  auto t = []() -> faio::task<long long> {
    const auto start = std::chrono::steady_clock::now();
    co_await faio::time::sleep(std::chrono::milliseconds(10));
    const auto end = std::chrono::steady_clock::now();
    co_return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  };

  const auto elapsed_ms = faio::block_on(ctx, t());
  EXPECT_GE(elapsed_ms, 8);
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

TEST(NetAddressTest, ParseHostname) {
  auto addr = faio::net::address::parse("localhost", 1234);
  ASSERT_TRUE(addr.has_value());
  EXPECT_EQ(addr->port(), 1234);
}
