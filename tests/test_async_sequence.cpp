#include <faio/coroutine/sequence.hpp>
#include <faio/runtime/instance.hpp>
#include <gtest/gtest.h>

namespace {
struct item {
  explicit item(int& live, int& high) : live_(&live) { high = std::max(high, ++live); }

  item(item&& other) noexcept : live_(std::exchange(other.live_, nullptr)) {}

  item& operator=(item&&) = delete;

  ~item() {
    if (live_)
      --*live_;
  }

  int* live_{};
};

faio::task<void> limited(std::size_t limit) {
  int live = 0, high = 0, done = 0;
  co_await faio::for_each_concurrent<item>(
      limit,
      [&](faio::stream_producer<item>& sink) -> faio::task<void> {
        for (int i = 0; i < 30; ++i)
          co_await sink.emit(item{live, high});
      },
      [&](item value) -> faio::task<void> {
        EXPECT_TRUE(value.live_);
        co_await faio::this_coro::yield();
        ++done;
      });
  EXPECT_EQ(done, 30);
  EXPECT_EQ(live, 0);
  EXPECT_LE(high, static_cast<int>(limit));
}

faio::task<void> fails() {
  std::size_t emitted = 0;
  try {
    co_await faio::for_each_concurrent<int>(
        2,
        [&](faio::stream_producer<int>& sink) -> faio::task<void> {
          while (true) {
            ++emitted;
            co_await sink.emit(3);
          }
        },
        [](int) -> faio::task<void> {
          throw std::runtime_error("operation");
          co_return;
        });
    ADD_FAILURE() << "operation failure was not propagated";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "operation");
  }
  EXPECT_LE(emitted, 2);
}

TEST(CoroutineAsyncSequence, BoundedMoveOnlyAndErrors) {
  faio::runtime::current_thread_runtime runtime;
  for (const auto n : {1u, 2u, 32u})
    runtime.block_on(limited(n));
  runtime.block_on(fails());
}
}  // namespace
