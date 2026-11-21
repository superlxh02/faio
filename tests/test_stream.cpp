#include <faio/coroutine/sequence.hpp>
#include <faio/runtime/instance.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using faio::stream_producer;
using faio::stream_reader;
using faio::with_stream;

faio::task<void> no_prefetch() {
  int produced = 0;
  co_await with_stream<std::unique_ptr<int>>(
      [&](stream_producer<std::unique_ptr<int>>& output) -> faio::task<void> {
        ++produced;
        co_await output.emit(std::make_unique<int>(5));
        ++produced;
        co_await output.emit(std::make_unique<int>(7));
        ++produced;
      },
      [&](stream_reader<std::unique_ptr<int>>& input) -> faio::task<void> {
        EXPECT_EQ(produced, 0);
        auto first = co_await input.next();
        EXPECT_EQ(**first, 5);
        EXPECT_EQ(produced, 1);
        auto second = co_await input.next();
        EXPECT_EQ(**second, 7);
        EXPECT_EQ(produced, 2);
        EXPECT_FALSE(co_await input.next());
        EXPECT_FALSE(co_await input.next());
        EXPECT_EQ(produced, 3);
      });
}

faio::task<void> early_exit() {
  std::atomic<bool> advanced_after_emit{};
  co_await with_stream<int>(
      [&](stream_producer<int>& output) -> faio::task<void> {
        co_await output.emit(9);
        advanced_after_emit = true;
      },
      [](stream_reader<int>& input) -> faio::task<void> {
        EXPECT_EQ((co_await input.next()).value(), 9);
      });
  EXPECT_FALSE(advanced_after_emit);
  try {
    co_await with_stream<int>(
        [](stream_producer<int>&) -> faio::task<void> {
          throw std::runtime_error("producer");
          co_return;
        },
        [](stream_reader<int>& input) -> faio::task<void> { (void)co_await input.next(); });
    ADD_FAILURE() << "missing producer error";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "producer");
  }
}

TEST(CoroutineStream, NoPrefetchMoveOnlyAndEof) {
  faio::runtime::current_thread_runtime runtime;
  runtime.block_on(no_prefetch());
}

TEST(CoroutineStream, EarlyExitAndUpstreamException) {
  faio::runtime::multi_thread_runtime runtime{faio::config_builder{}.set_num_workers(4).build()};
  runtime.block_on(early_exit());
}
}  // namespace
