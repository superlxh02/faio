#include "faio/faio.hpp"
#include "faio/detail/runtime/common/external_work.hpp"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {
using faio::detail::external_work_host;
using faio::detail::external_work_node;
struct count_node : external_work_node {
  std::atomic<unsigned>* count{};
  static void run(external_work_node* node) noexcept {
    static_cast<count_node*>(node)->count->fetch_add(1, std::memory_order_release);
  }
  explicit count_node(std::atomic<unsigned>& counter) : count(&counter) { execute = run; }
};

TEST(ExternalHost, TopReservationRejectsShutdownWithoutClosingAdmission) {
  external_work_host host;
  auto root = host.acquire_root();
  auto reservation = host.reserve(root, true);
  EXPECT_THROW(host.begin_quiescing(), std::logic_error);
  EXPECT_EQ(host.state(), external_work_host::phase::accepting);
  auto other = host.acquire_root();
  other->terminal();
  reservation.reset();
  host.begin_quiescing();
  EXPECT_THROW(host.acquire_root(), std::logic_error);
  root->terminal();
  EXPECT_TRUE(host.quiescent());
}

TEST(ExternalHost, RetiredMetadataCanOutliveDestroyedHost) {
  external_work_host::root_handle metadata;
  {
    external_work_host host;
    metadata = host.acquire_root();
    metadata->terminal();
    EXPECT_TRUE(metadata->drained());
    EXPECT_TRUE(host.quiescent());
    host.mark_stopped();
  }
  EXPECT_TRUE(metadata->drained());
  metadata.reset();  // 已退役元数据析构不可再访问 host。
}

TEST(ExternalHost, QuiescingKeepsAcceptedInternalOperations) {
  external_work_host host;
  auto root = host.acquire_root();
  host.begin_quiescing();
  auto reservation = host.reserve(root);
  root->terminal();
  EXPECT_FALSE(root->drained());
  reservation.start();
  EXPECT_TRUE(root->drained());
  EXPECT_TRUE(host.quiescent());
}

TEST(ExternalHost, PublisherAndChildPreventRetirement) {
  external_work_host host;
  auto root = host.acquire_root();
  root->child_begin();
  {
    external_work_host::publisher_guard publisher{root};
    root->terminal();
    root->child_end();
    EXPECT_FALSE(root->drained());
    EXPECT_FALSE(host.quiescent());
  }
  EXPECT_TRUE(root->drained());
  EXPECT_TRUE(host.quiescent());
}

TEST(ExternalHost, ChildLeaseTransfersAndReleasesExactlyOneResponsibility) {
  external_work_host host;
  auto root = host.acquire_root();
  auto first = host.acquire_child({&host, root.get()});
  auto second = host.acquire_child({&host, root.get()});
  root->terminal();
  EXPECT_FALSE(root->drained());
  second = std::move(first);  // 先释放 second 的一票，再转移 first 的一票。
  EXPECT_FALSE(first);
  EXPECT_TRUE(second);
  EXPECT_FALSE(root->drained());
  second.reset();
  EXPECT_FALSE(second);
  EXPECT_TRUE(root->drained());
  EXPECT_TRUE(host.quiescent());
  EXPECT_FALSE(host.acquire_child({&host, root.get()}));
}

TEST(ExternalHost, PublishWakerTailKeepsGraphAliveAfterReceiverFinished) {
  external_work_host host;
  auto root = host.acquire_root();
  std::atomic<unsigned> count{};
  count_node node{count};
  node.scope = {&host, root.get()};
  struct wake_state {
    external_work_host* host;
    external_work_host::root_handle root;
    bool held{};
    bool first{true};
  } state{&host, root};
  host.set_waker(&state, +[](void* pointer, std::size_t worker) noexcept {
    auto& state = *static_cast<wake_state*>(pointer);
    if (!std::exchange(state.first, false))
      return;
    {
      auto dispatch = state.host->try_pop_external(worker);
      auto* node = dispatch.node();
      state.root->terminal();
      node->execute(node);
    }
    state.held = !state.root->drained();
  });
  host.publish(node);
  EXPECT_TRUE(state.held);
  EXPECT_TRUE(root->drained());
  EXPECT_TRUE(host.quiescent());
}

TEST(ExternalHost, WakerTailWaitSettlesQuiescenceAndExcludesUserReceiverGuards) {
  external_work_host host;
  struct wake_state {
    std::atomic<bool> entered{}, release{}, waiter_ready{};
  } state;
  host.set_waker(&state, +[](void* pointer, std::size_t) noexcept {
    auto& state = *static_cast<wake_state*>(pointer);
    state.entered.store(true, std::memory_order_release);
    state.entered.notify_all();
    state.release.wait(false, std::memory_order_acquire);
  });
  std::thread publisher{[&] { host.notify_completion(); }};
  state.entered.wait(false, std::memory_order_acquire);
  EXPECT_FALSE(host.quiescent());  // The kernel wake tail still owns its ticket.
  std::thread release{[&] {
    state.waiter_ready.wait(false, std::memory_order_acquire);
    state.release.store(true, std::memory_order_release);
    state.release.notify_all();
  }};
  state.waiter_ready.store(true, std::memory_order_release);
  state.waiter_ready.notify_all();
  // If release wins the snapshot race this returns immediately. Otherwise it
  // waits for the tail's revision signal; both interleavings must settle safely.
  (void)host.wait_for_wake_publisher();
  publisher.join();
  release.join();
  EXPECT_TRUE(host.quiescent());
  host.set_waker(nullptr, nullptr);
  auto root = host.acquire_root();
  {
    external_work_host::publisher_guard receiver{root};
    root->terminal();
    EXPECT_FALSE(host.quiescent());
    EXPECT_FALSE(host.wait_for_wake_publisher());
  }
  EXPECT_TRUE(host.quiescent());
}

TEST(ExternalHost, DispatchSurvivesSelfDeletingNodeAndTriggersCleanupOnce) {
  external_work_host host;
  auto root = host.acquire_root();
  std::atomic<unsigned> called{}, cleanup{};
  count_node cleanup_node{cleanup};
  root->on_drained(cleanup_node);
  struct self_deleting_node : external_work_node {
    std::atomic<unsigned>* called;
    explicit self_deleting_node(std::atomic<unsigned>& counter) : called(&counter) {
      execute = +[](external_work_node* node) noexcept {
        auto* self = static_cast<self_deleting_node*>(node);
        auto* counter = self->called;
        delete self;
        counter->fetch_add(1, std::memory_order_release);
      };
    }
  };
  auto* node = new self_deleting_node{called};
  node->scope = {&host, root.get()};
  host.publish(*node);
  root->terminal();
  {
    auto dispatch = host.try_pop_external(0);
    auto* selected = dispatch.node();
    selected->execute(selected);
    EXPECT_EQ(called.load(), 1u);
    EXPECT_FALSE(root->drained());
  }
  EXPECT_TRUE(root->drained());
  EXPECT_FALSE(host.quiescent());
  {
    auto dispatch = host.try_pop_external(0);
    ASSERT_TRUE(dispatch);
    auto* selected = dispatch.node();
    EXPECT_FALSE(selected->scope);
    selected->execute(selected);
  }
  EXPECT_EQ(cleanup.load(), 1u);
  EXPECT_FALSE(host.try_pop_external(0));
  EXPECT_TRUE(host.quiescent());
}

TEST(ExternalHost, CleanupInstalledAfterRetirementIsQueued) {
  external_work_host host;
  auto root = host.acquire_root();
  root->terminal();
  std::atomic<unsigned> count{};
  count_node node{count};
  root->on_drained(node);
  {
    auto dispatch = host.try_pop_external(0);
    auto* selected = dispatch.node();
    selected->execute(selected);
  }
  EXPECT_EQ(count.load(), 1u);
  EXPECT_TRUE(host.quiescent());
}

TEST(ExternalHost, StopCallbacksRunOutsideAdmissionLock) {
  external_work_host host;
  auto root = host.acquire_root();
  bool called = false;
  std::stop_callback callback{root->stop_token(), [&] {
    auto nested = host.acquire_root();
    called = nested->stop_token().stop_requested();
    nested->terminal();
  }};
  host.request_stop();
  EXPECT_TRUE(called);
  root->terminal();
}

TEST(ExternalHost, RemotePublicationDrivesBothExistingRuntimeModes) {
  for (auto mode : {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
    auto config = faio::runtime::detail::runtime_config{};
    config._mode = mode;
    config._num_workers = mode == faio::runtime::mode::current_thread ? 1 : 3;
#if defined(__linux__)
    config._requested_io_backend = faio::runtime::io_backend::IO_EPOLL;
#endif
    faio::runtime::detail::runtime_context context{config};
    auto& host = context.external_host();
    auto root = host.acquire_root();
    std::atomic<unsigned> count{};
    std::vector<std::unique_ptr<count_node>> nodes;
    for (unsigned i = 0; i < 256; ++i) {
      nodes.push_back(std::make_unique<count_node>(count));
      nodes.back()->scope = {&host, root.get()};
    }
    std::thread producer{[&] {
      for (auto& node : nodes)
        host.publish(*node);
      root->terminal();
      host.notify_completion();
    }};
    context.drive_until([&] { return count.load(std::memory_order_acquire) == nodes.size() && root->drained(); });
    producer.join();
    EXPECT_EQ(count.load(), nodes.size());
    context.shutdown();
  }
}

TEST(ExternalHost, CompletionWithoutNodeWakesSleepingCurrentDriver) {
  auto config = faio::runtime::detail::runtime_config{};
  config._mode = faio::runtime::mode::current_thread;
  config._num_workers = 1;
#if defined(__linux__)
  config._requested_io_backend = faio::runtime::io_backend::IO_EPOLL;
#endif
  faio::runtime::detail::runtime_context context{config};
  auto root = context.external_host().acquire_root();
  std::thread completion{[&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    root->terminal();
  }};
  context.drive_until([&] { return root->drained(); });
  completion.join();
  context.shutdown();
}

TEST(ExternalHost, WorkerPublicationStaysOnItsOwnExternalFifo) {
  auto config = faio::runtime::detail::runtime_config{};
  config._num_workers = 3;
#if defined(__linux__)
  config._requested_io_backend = faio::runtime::io_backend::IO_EPOLL;
#endif
  faio::runtime::detail::runtime_context context{config};
  auto root = context.external_host().acquire_root();
  struct chain_node : external_work_node {
    external_work_host* host;
    external_work_host::root_handle root;
    std::atomic<unsigned> calls{};
    std::size_t first_worker{};
    bool stayed{};
    chain_node(external_work_host& target, external_work_host::root_handle graph)
        : host(&target), root(std::move(graph)) {
      scope = {host, root.get()};
      execute = +[](external_work_node* node) noexcept {
        auto& self = *static_cast<chain_node*>(node);
        if (self.calls.load(std::memory_order_relaxed) == 0) {
          self.first_worker = faio::detail::current_worker_id();
          self.calls.store(1, std::memory_order_relaxed);
          self.host->publish(self);
        } else {
          self.stayed = self.first_worker == faio::detail::current_worker_id();
          self.root->terminal();
          self.calls.store(2, std::memory_order_release);
        }
      };
    }
  } node{context.external_host(), root};
  context.external_host().publish(node);
  context.drive_until([&] { return node.calls.load(std::memory_order_acquire) == 2 && root->drained(); });
  EXPECT_TRUE(node.stayed);
  context.shutdown();
}

faio::task<void> timer_and_coroutine_progress(std::atomic<unsigned>* steps) {
  co_await faio::time::sleep(std::chrono::milliseconds{1});
  for (unsigned i = 0; i < 64; ++i) {
    steps->fetch_add(1, std::memory_order_relaxed);
    co_await faio::this_coro::yield();
  }
}
TEST(ExternalHost, ContinuousExternalWorkAllowsTimerAndCoroutineProgress) {
  for (auto mode : {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
    auto config = faio::runtime::detail::runtime_config{};
    config._mode = mode;
    config._num_workers = 1;
    config._io_interval = 2;
    config._global_queue_interval = 2;
#if defined(__linux__)
    config._requested_io_backend = faio::runtime::io_backend::IO_EPOLL;
#endif
    faio::runtime::detail::runtime_context context{config};
    auto root = context.external_host().acquire_root();
    struct busy_node : external_work_node {
      external_work_host* host;
      external_work_host::root_handle root;
      std::atomic<bool> running{true};
      std::atomic<unsigned> turns{};
      busy_node(external_work_host& target, external_work_host::root_handle graph)
          : host(&target), root(std::move(graph)) {
        scope = {host, root.get()};
        execute = +[](external_work_node* base) noexcept {
          auto& node = *static_cast<busy_node*>(base);
          node.turns.fetch_add(1, std::memory_order_relaxed);
          if (node.running.load(std::memory_order_acquire))
            node.host->publish(node);
          else
            node.root->terminal();
        };
      }
    } busy{context.external_host(), root};
    std::atomic<unsigned> steps{};
    context.external_host().publish(busy);
    context.block_on(timer_and_coroutine_progress(&steps));
    EXPECT_EQ(steps.load(), 64u);
    EXPECT_GT(busy.turns.load(), 0u);
    busy.running.store(false, std::memory_order_release);
    context.drive_until([&] { return root->drained(); });
    context.shutdown();
  }
}
}  // namespace
