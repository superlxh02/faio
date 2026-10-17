#include "faio/detail/fs/core/active_operations.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace {
using registry = faio::fs::detail::active_file_operations;

TEST(FileOperationRegistry, BarrierSnapshotOwnsOnlyPreviouslyAcceptedWrites) {
  registry active;
  auto first_write = std::make_shared<registry::operation>(true);
  auto read = std::make_shared<registry::operation>(false);
  active.insert(first_write);
  active.insert(read);

  auto barrier = active.capture(true);
  ASSERT_EQ(barrier.size(), 1u);
  EXPECT_EQ(barrier.front(), first_write);
  EXPECT_FALSE(barrier.front()->completion.wait().await_ready());

  auto later_write = std::make_shared<registry::operation>(true);
  active.insert(later_write);
  const std::weak_ptr<registry::operation> completed = first_write;
  active.remove(first_write);
  first_write->completion.notify();
  first_write.reset();

  // A pending barrier keeps its completed event alive, but the event must no
  // longer appear among active requests or in a later close/flush snapshot.
  EXPECT_FALSE(completed.expired());
  EXPECT_TRUE(barrier.front()->completion.wait().await_ready());
  auto next_barrier = active.capture(true);
  ASSERT_EQ(next_barrier.size(), 1u);
  EXPECT_EQ(next_barrier.front(), later_write);
  EXPECT_FALSE(next_barrier.front()->completion.wait().await_ready());
  auto close_snapshot = active.capture();
  ASSERT_EQ(close_snapshot.size(), 2u);
  EXPECT_EQ(barrier.size(), 1u);

  active.remove(read);
  read->completion.notify();
  active.remove(later_write);
  later_write->completion.notify();
  EXPECT_TRUE(active.empty());
  for (const auto& operation : close_snapshot)
    EXPECT_TRUE(operation->completion.wait().await_ready());
  barrier.clear();
  EXPECT_TRUE(completed.expired());
}

TEST(FileOperationRegistry, OutOfOrderCompletionKeepsAllRemainingRegistrationsValid) {
  registry active;
  std::vector<registry::handle> operations;
  constexpr std::size_t count = 257;
  for (std::size_t index = 0; index < count; ++index) {
    auto operation = std::make_shared<registry::operation>(index % 2 == 0);
    active.insert(operation);
    operations.push_back(std::move(operation));
  }
  auto snapshot = active.capture();
  std::size_t remaining = count;
  // Alternate front/back completion; moved registrations must remain removable
  // even while a close snapshot still retains every original operation.
  for (std::size_t index = 0; index < (count + 1) / 2; ++index) {
    active.remove(operations[index]);
    operations[index]->completion.notify();
    EXPECT_EQ(active.size(), --remaining);
    const auto opposite = count - 1 - index;
    if (opposite != index) {
      active.remove(operations[opposite]);
      operations[opposite]->completion.notify();
      EXPECT_EQ(active.size(), --remaining);
    }
  }
  EXPECT_TRUE(active.empty());
  EXPECT_TRUE(active.capture().empty());
  for (const auto& operation : snapshot)
    EXPECT_TRUE(operation->completion.wait().await_ready());
}
}  // namespace
