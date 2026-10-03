/**
 * @file test_worker_io_budget.cpp
 * @brief R2 的有限 fake-clock 边界：旧锚点不前移，只有真实计时驱动可重置预算。
 * @details 不启动 backend/线程，不 sleep；使用生产 helper 验证回调与时间决定。
 */
#include "faio/detail/runtime/common/worker_io_budget.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
using faio::runtime::detail::worker_io_budget;
struct fake_clock {
  worker_io_budget::time_point value{};
  unsigned reads{};
  auto read() noexcept {
    ++reads;
    return value;
  }
  void advance(worker_io_budget::duration elapsed) noexcept {
    value += elapsed;
  }
};

TEST(WorkerIoBudget,
     EmptyIdleReadsNoClockAndWithinBudgetResumeNeedsNoExtraDrive) {
  fake_clock clock;
  worker_io_budget budget{clock.value};
  unsigned actual_drives{};
  for (unsigned turn = 0; turn < 32; ++turn) {
    clock.advance(1us);
    EXPECT_FALSE(budget.drive_without_anchor([&] {
      ++actual_drives;
      return false;
    }));
  }
  EXPECT_EQ(actual_drives, 32u); // 所有空轮询仍真正执行，不能省掉原 IO 工作。
  EXPECT_EQ(clock.reads, 0u);
  EXPECT_FALSE(budget.refresh_before_execute(
      100us, [&] { return clock.read(); }, [&] { ++actual_drives; }));
  EXPECT_EQ(clock.reads, 1u);
  EXPECT_EQ(actual_drives,
            32u); // 未到旧预算时，不像 R 那样强制 idle→active 再poll。
}

TEST(WorkerIoBudget, RecentIdleCannotMoveOldAnchorOrHideExactExpiry) {
  fake_clock clock;
  worker_io_budget budget{clock.value};
  unsigned actual_drives{};
  clock.advance(99us);
  budget.drive_without_anchor(
      [&] { ++actual_drives; }); // 刚poll也不重置0us旧锚点。
  clock.advance(1us);
  ASSERT_TRUE(budget.refresh_before_execute(
      100us, [&] { return clock.read(); }, [&] { ++actual_drives; }));
  EXPECT_EQ(actual_drives, 2u);
  clock.advance(99us);
  EXPECT_FALSE(budget.refresh_before_execute(
      100us, [&] { return clock.read(); }, [&] { ++actual_drives; }));
  clock.advance(1us);
  EXPECT_TRUE(budget.refresh_before_execute(
      100us, [&] { return clock.read(); }, [&] { ++actual_drives; }));
  EXPECT_EQ(actual_drives,
            3u); // 只有真实timed drive在100us重置，所以200us精确到期。
}

TEST(WorkerIoBudget, ContinuouslyReadyKeepsOriginalElapsedAndZeroBoundary) {
  fake_clock clock;
  worker_io_budget budget{clock.value};
  unsigned actual_drives{};
  auto refresh = [&](worker_io_budget::duration limit) {
    return budget.refresh_before_execute(
        limit, [&] { return clock.read(); }, [&] { ++actual_drives; });
  };
  clock.advance(99us);
  EXPECT_FALSE(refresh(100us));
  clock.advance(1us);
  EXPECT_TRUE(refresh(100us));
  clock.advance(99us);
  EXPECT_FALSE(refresh(100us));
  clock.advance(1us);
  EXPECT_TRUE(refresh(100us));
  EXPECT_TRUE(refresh(0us)); // 相等始终需要驱动，零预算不能被放宽。
  EXPECT_EQ(actual_drives, 3u);
  EXPECT_EQ(clock.reads, 5u);
}

TEST(WorkerIoBudget, LongUntimedDriveStillExpiresBeforeSelectedTaskResume) {
  fake_clock clock;
  worker_io_budget budget{clock.value};
  std::vector<int> order;
  budget.drive_without_anchor([&] {
    order.push_back(1);
    clock.advance(
        250us); // 空闲drive本身也可能长耗时，不允许在结束后补伪时间戳。
  });
  ASSERT_TRUE(budget.refresh_before_execute(
      100us, [&] { return clock.read(); }, [&] { order.push_back(2); }));
  order.push_back(3); // 原drive及超预算timed drive都结束后才恢复用户代码。
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(WorkerIoBudget, LongTimedDriveDurationIsNotHiddenByEndTimestamp) {
  fake_clock clock;
  worker_io_budget budget{clock.value};
  unsigned actual_drives{};
  budget.drive_with_anchor([&] { return clock.read(); },
                           [&] {
                             ++actual_drives;
                             clock.advance(250us);
                           });
  ASSERT_TRUE(budget.refresh_before_execute(
      100us, [&] { return clock.read(); }, [&] { ++actual_drives; }));
  EXPECT_EQ(actual_drives,
            2u); // timed anchor只能在0us开始前采样，不能移到250us结束时。
}

TEST(WorkerIoBudget, PreemptionDuringStealSelectionUsesOldBudgetBeforeResume) {
  fake_clock clock;
  worker_io_budget budget{clock.value};
  std::vector<int> order;
  clock.advance(20us);
  budget.drive_without_anchor([&] { order.push_back(1); });
  clock.advance(500us); // 模拟idle poll以后、steal选中任务期间的线程抢占。
  ASSERT_TRUE(budget.refresh_before_execute(
      100us, [&] { return clock.read(); }, [&] { order.push_back(2); }));
  order.push_back(3);
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(clock.reads, 1u); // 不读idle时钟也不能绕过恢复边界的真实sample。
}

TEST(WorkerIoBudget,
     PeriodicDriveReanchorsOnlyAlongActualDriveAndPreservesProgress) {
  fake_clock clock;
  worker_io_budget budget{clock.value};
  unsigned actual_drives{};
  clock.advance(90us);
  EXPECT_TRUE(budget.drive_with_anchor([&] { return clock.read(); },
                                       [&] {
                                         ++actual_drives;
                                         return true;
                                       }));
  clock.advance(20us);
  EXPECT_FALSE(budget.refresh_before_execute(
      100us, [&] { return clock.read(); }, [&] { ++actual_drives; }));
  clock.advance(80us);
  EXPECT_TRUE(budget.refresh_before_execute(
      100us, [&] { return clock.read(); }, [&] { ++actual_drives; }));
  EXPECT_EQ(actual_drives, 2u); // period真实驱动开始90us，下一精确边界是190us。
}

TEST(WorkerIoBudget,
     ExpiredDriveRequiresShutdownRecheckBeforeLocalOrStealResume) {
  for (const bool stolen : {false, true}) {
    fake_clock clock;
    worker_io_budget budget{clock.value};
    bool closed = false;
    unsigned resumes{};
    budget.drive_without_anchor([] {});
    clock.advance(100us);
    const bool drove = budget.refresh_before_execute(
        100us, [&] { return clock.read(); }, [&] { closed = true; });
    EXPECT_TRUE(drove);
    if (!(drove && closed))
      ++resumes; // 两种来源的同一worker关闭复查条件。
    EXPECT_EQ(resumes, 0u) << "stolen=" << stolen;
  }
}
} // namespace
