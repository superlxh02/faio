#include "faio/detail/io/backend/factory.hpp"
/**
 * @file test_reactor_contract.cpp
 * @brief epoll/kqueue 共同的后端契约：就绪、半关闭、控制唤醒及长期注册。
 * @details 本测试直接使用后端中立静态接口，不依赖 runtime 或协程。
 */
#include "faio/detail/io/platform/reactor.hpp"
#include "faio/detail/io/base/readiness_adapter.hpp"
#include <gtest/gtest.h>
#if defined(__linux__)
#include "faio/detail/io/backend/epoll/reactor.hpp"
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include "faio/detail/io/backend/kqueue/reactor.hpp"
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <set>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
namespace faio::io::detail {
struct reactor_control_test_access {
  template <class R>
  static void bind(R& reactor,
                   void (*step)(reactor_control_test_point, void*) noexcept,
                   void* context) noexcept {
    reactor.control_test_step_ = step;
    reactor.control_test_context_ = context;
  }
};
}  // namespace faio::io::detail
#endif

namespace {
using namespace std::chrono_literals;
using faio::io::detail::readiness_event;

#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
using faio::io::detail::reactor_control_test_access;
using faio::io::detail::reactor_control_test_point;
#if defined(__linux__)
using concrete_reactor = faio::io::detail::epoll_reactor;
#else
using concrete_reactor = faio::io::detail::kqueue_reactor;
#endif

/** @brief 一次可控交接；钩子和测试等待都限时，失败不遗留阻塞的 jthread。 */
class controlled_reactor_pause {
 public:
  explicit controlled_reactor_pause(reactor_control_test_point point) : point_(point) {}

  static void step(reactor_control_test_point point, void* context) noexcept {
    auto& self = *static_cast<controlled_reactor_pause*>(context);
    if (point != self.point_)
      return;
    std::unique_lock lock(self.mutex_);
    if (!self.armed_)
      return;
    self.armed_ = false;
    self.entered_ = true;
    self.changed_.notify_all();
    if (!self.changed_.wait_for(lock, 2s, [&] { return self.released_; }))
      self.timed_out_ = true;
  }

  bool wait_for_entry() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 1s, [&] { return entered_; });
  }

  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

  bool timed_out() {
    std::lock_guard lock(mutex_);
    return timed_out_;
  }

 private:
  const reactor_control_test_point point_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool armed_{true}, entered_{}, released_{}, timed_out_{};
};

// Declare after the worker thread so ASSERT failure releases the hook before
// jthread's destructor joins that worker.
struct release_reactor_pause {
  controlled_reactor_pause& pause;

  ~release_reactor_pause() { pause.release(); }
};
#endif

/** @brief 只用于测试的描述符所有者；资源销毁晚于 reactor.unregister_resource。 */
struct descriptor_pair {
  std::array<int, 2> fds{-1, -1};

  explicit descriptor_pair(bool pipe = false) {
    const auto result =
        pipe ? ::pipe(fds.data()) : ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data());
    if (result < 0)
      throw std::system_error(errno, std::generic_category());
    for (const int fd : fds) {
      if (::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0)
        throw std::system_error(errno, std::generic_category());
    }
  }

  descriptor_pair(const descriptor_pair&) = delete;

  descriptor_pair& operator=(const descriptor_pair&) = delete;

  ~descriptor_pair() {
    for (const int fd : fds)
      if (fd >= 0)
        ::close(fd);
  }
};

/** @brief 等待指定token的指定方向；读/写独立事件不要求合并在同一元素。 */
bool wait_flag(faio::io::detail::reactor_box& reactor, std::uint64_t key, std::uint32_t flags) {
  std::array<readiness_event, 16> events{};
  for (int attempt = 0; attempt < 5; ++attempt) {
    const int count = reactor.poll(events, 100);
    if (count < 0)
      return false;
    for (int i = 0; i < count; ++i)
      if (events[static_cast<std::size_t>(i)].key == key
          && (events[static_cast<std::size_t>(i)].flags & flags))
        return true;
  }
  return false;
}
}  // namespace

TEST(ReactorContract, EmptyOutputIsNonBlockingNoOp) {
  auto reactor = faio::io::detail::make_platform_reactor();
  EXPECT_EQ(reactor.poll({}, std::nullopt), 0);
}

TEST(ReactorContract, EmptyQueueHonorsFiniteTimeout) {
  auto reactor = faio::io::detail::make_platform_reactor();
  std::array<readiness_event, 16> events{};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(reactor.poll(events, 10), 0);
  EXPECT_GE(std::chrono::steady_clock::now() - start, 7ms);
}

TEST(ReactorContract, ControlWakeInterruptsWaitingDriver) {
  auto reactor = faio::io::detail::make_platform_reactor();
  std::array<readiness_event, 16> events{};
  std::jthread producer([&] {
    std::this_thread::sleep_for(5ms);
    reactor.wake();
  });
  const auto start = std::chrono::steady_clock::now();
  const int count = reactor.poll(events, 500);
  EXPECT_GT(count, 0);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 450ms);
  bool control = false;
  for (int i = 0; i < count; ++i)
    if (events[static_cast<std::size_t>(i)].key == 0)
      control = true;
  EXPECT_TRUE(control);
}

TEST(ReactorContract, StableSocketRegistrationReportsBothDirectionsAndHalfClose) {
  descriptor_pair pair;
  auto reactor = faio::io::detail::make_platform_reactor();
  constexpr std::uint64_t generation = 0x1'0000'0001ULL;
  ASSERT_EQ(reactor.register_resource(pair.fds[0], generation), 0);
  EXPECT_TRUE(wait_flag(reactor, generation, faio::io::detail::writable_bit));
  const char byte = 'x';
  ASSERT_EQ(::write(pair.fds[1], &byte, 1), 1);
  EXPECT_TRUE(wait_flag(reactor, generation, faio::io::detail::readable_bit));
  char received{};
  ASSERT_EQ(::read(pair.fds[0], &received, 1), 1);
  EXPECT_EQ(received, byte);
  ASSERT_EQ(::shutdown(pair.fds[1], SHUT_WR), 0);
  EXPECT_TRUE(wait_flag(reactor, generation, faio::io::detail::closed_bit));
  EXPECT_EQ(::read(pair.fds[0], &received, 1), 0);
  reactor.unregister_resource(pair.fds[0]);
}

TEST(ReactorContract, ReadOnlyPipeRegistrationDoesNotRequireWriteFilter) {
  descriptor_pair pair{true};
  auto reactor = faio::io::detail::make_platform_reactor();
  ASSERT_EQ(reactor.register_resource(pair.fds[0], 17), 0);
  const char byte = 'p';
  ASSERT_EQ(::write(pair.fds[1], &byte, 1), 1);
  EXPECT_TRUE(wait_flag(reactor, 17, faio::io::detail::readable_bit));
  reactor.unregister_resource(pair.fds[0]);
}

TEST(ReactorContract, InvalidDescriptorFailsWithoutPreventingLaterRegistration) {
  auto reactor = faio::io::detail::make_platform_reactor();
  EXPECT_LT(reactor.register_resource(-1, 1), 0);
  descriptor_pair pair;
  ASSERT_EQ(reactor.register_resource(pair.fds[0], 2), 0);
  EXPECT_TRUE(wait_flag(reactor, 2, faio::io::detail::writable_bit));
  reactor.unregister_resource(pair.fds[0]);
}

TEST(ReactorContract, ReregisterPublishesNewGenerationAndUnregisterDiscardsEvents) {
  descriptor_pair pair;
  auto reactor = faio::io::detail::make_platform_reactor();
  ASSERT_EQ(reactor.register_resource(pair.fds[0], 19), 0);
  reactor.unregister_resource(pair.fds[0]);
  ASSERT_EQ(reactor.register_resource(pair.fds[0], 20), 0);
  EXPECT_TRUE(wait_flag(reactor, 20, faio::io::detail::writable_bit));
  reactor.unregister_resource(pair.fds[0]);
  std::array<readiness_event, 16> events{};
  EXPECT_EQ(reactor.poll(events, 0), 0);
}

TEST(ReactorContract, ConcurrentWakeBurstsRemainBoundedAndDrainable) {
  auto reactor = faio::io::detail::make_platform_reactor();
  std::vector<std::jthread> producers;
  for (int i = 0; i < 4; ++i)
    producers.emplace_back([&] {
      for (int repeat = 0; repeat < 1000; ++repeat)
        reactor.wake();
    });
  producers.clear();
  std::array<readiness_event, 16> events{};
  EXPECT_GT(reactor.poll(events, 100), 0);
  EXPECT_EQ(reactor.poll(events, 0), 0);
}

TEST(ReactorContract, WakeAfterEachPollBoundaryPublishesEveryRound) {
  auto reactor = faio::io::detail::make_platform_reactor();
  std::atomic<unsigned> published{}, acknowledged{};
  constexpr unsigned rounds = 2000;
  std::jthread producer([&](std::stop_token stop) {
    for (unsigned round = 1; round <= rounds && !stop.stop_requested(); ++round) {
      while (acknowledged.load(std::memory_order_acquire) != round - 1) {
        if (stop.stop_requested())
          return;
        std::this_thread::yield();
      }
      published.store(round, std::memory_order_release);
      reactor.wake();
      reactor.wake();  // The same epoch may coalesce, including across a kernel drain.
    }
  });
  std::array<readiness_event, 16> events{};
  for (unsigned round = 1; round <= rounds; ++round) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (published.load(std::memory_order_acquire) < round
           && std::chrono::steady_clock::now() < deadline)
      ASSERT_GE(reactor.poll(events, 20), 0);
    ASSERT_EQ(published.load(std::memory_order_acquire), round);
    acknowledged.store(round, std::memory_order_release);
  }
}

#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
TEST(ReactorContract, NoticeBeforeDelayedControlSyscallPreventsBlocking) {
  concrete_reactor reactor;
  controlled_reactor_pause pause{reactor_control_test_point::wake_before_syscall};
  reactor_control_test_access::bind(reactor, &controlled_reactor_pause::step, &pause);
  std::jthread producer([&] { reactor.wake(); });
  release_reactor_pause release{pause};
  ASSERT_TRUE(pause.wait_for_entry());

  // The producer published the epoch but has not written/triggered an OS event.
  // A finite timeout bounds failure; the epoch must still force a zero wait.
  std::array<readiness_event, 16> events{};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(reactor.poll(events, 500), 0);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);

  pause.release();
  producer.join();
  EXPECT_FALSE(pause.timed_out());
  EXPECT_GT(reactor.poll(events, 100), 0);  // The delayed syscall remains drainable.
  EXPECT_EQ(reactor.poll(events, 0), 0);
}

TEST(ReactorContract, NewNoticeSurvivesControlEventConsumedByPreviousPoll) {
  concrete_reactor reactor;
  reactor.wake();
  controlled_reactor_pause pause{reactor_control_test_point::poll_after_consume};
  reactor_control_test_access::bind(reactor, &controlled_reactor_pause::step, &pause);
  std::array<readiness_event, 16> events{};
  int first_count{};
  std::jthread consumer([&] { first_count = reactor.poll(events, 500); });
  release_reactor_pause release{pause};
  ASSERT_TRUE(pause.wait_for_entry());

  // This wake belongs to the next epoch, but its OS event is deliberately
  // consumed by the current epoll drain / EV_CLEAR retrieval.
  reactor.wake();
  pause.release();
  consumer.join();
  EXPECT_FALSE(pause.timed_out());
  ASSERT_GT(first_count, 0);

  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(reactor.poll(events, 500), 0);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);
  EXPECT_EQ(reactor.poll(events, 0), 0);  // The notice is consumed exactly once.
}
#endif

TEST(ReactorContract, AdapterMapsDirectlyWithoutLosingGenerationDirectionOrBudget) {
  using namespace faio::io::detail;
  static_assert(basic_readiness_adapter<reactor_box>::poll_flushes_submissions);
  descriptor_pair pair;
  basic_readiness_adapter adapter{make_platform_reactor()};
  constexpr std::uint64_t generation = 0xf123'4567'89ab'cdefULL;
  ASSERT_EQ(adapter.register_resource(pair.fds[0], generation), 0);
  EXPECT_EQ(adapter.poll({}, std::nullopt), 0);
  ASSERT_EQ(::write(pair.fds[1], "x", 1), 1);
  ASSERT_EQ(::shutdown(pair.fds[1], SHUT_WR), 0);
  std::array<backend_event, 1> events{};
  std::uint32_t flags{};
  for (unsigned attempt = 0; attempt < 8 && !(flags & read_closed_bit); ++attempt) {
    const int count = adapter.poll(events, 20);
    ASSERT_GE(count, 0);
    ASSERT_LE(count, 1);
    if (!count)
      continue;
    EXPECT_EQ(events[0].kind, backend_event_kind::readiness);
    EXPECT_EQ(events[0].key, generation);
    EXPECT_EQ(events[0].result, 0);
    flags |= events[0].flags;
  }
  EXPECT_NE(flags & readable_bit, 0u);
  EXPECT_NE(flags & read_closed_bit, 0u);
  adapter.unregister_resource(pair.fds[0]);
}

/** @brief 小输出 span 不能丢失内核 batch 中尚未移交的其他资源事件。 */
TEST(ReactorContract, SingleEventOutputEventuallyReportsEveryReadyResource) {
  auto reactor = faio::io::detail::make_platform_reactor();
  std::vector<std::unique_ptr<descriptor_pair>> pairs;
  constexpr std::size_t count = 80;
  for (std::size_t i = 0; i < count; ++i) {
    pairs.push_back(std::make_unique<descriptor_pair>());
    ASSERT_EQ(reactor.register_resource(pairs.back()->fds[0], i + 1), 0);
    const char byte = 'b';
    ASSERT_EQ(::write(pairs.back()->fds[1], &byte, 1), 1);
  }
  std::set<std::uint64_t> observed;
  std::array<readiness_event, 1> event{};
  for (std::size_t attempt = 0; attempt < count * 4 && observed.size() < count; ++attempt) {
    ASSERT_GE(reactor.poll(event, 20), 0);
    if (event[0].key && (event[0].flags & faio::io::detail::readable_bit))
      observed.insert(event[0].key);
    event[0] = {};
  }
  EXPECT_EQ(observed.size(), count);
  for (auto& pair : pairs)
    reactor.unregister_resource(pair->fds[0]);
}

/** @brief EOF 提示不能覆盖尚未读取的数据；readiness 只表达状态提示。 */
TEST(ReactorContract, HalfCloseRetainsUnreadPayload) {
  descriptor_pair pair;
  auto reactor = faio::io::detail::make_platform_reactor();
  ASSERT_EQ(reactor.register_resource(pair.fds[0], 33), 0);
  constexpr std::array<char, 4> bytes{'d', 'a', 't', 'a'};
  ASSERT_EQ(::write(pair.fds[1], bytes.data(), bytes.size()), 4);
  ASSERT_EQ(::shutdown(pair.fds[1], SHUT_WR), 0);
  EXPECT_TRUE(
      wait_flag(reactor, 33, faio::io::detail::readable_bit | faio::io::detail::closed_bit));
  std::array<char, 4> received{};
  EXPECT_EQ(::read(pair.fds[0], received.data(), received.size()), 4);
  EXPECT_EQ(received, bytes);
  EXPECT_EQ(::read(pair.fds[0], received.data(), received.size()), 0);
  reactor.unregister_resource(pair.fds[0]);
}

/**
 * @brief 本地写半关闭只影响写过滤器，不能伪造没有数据的读就绪。
 * @details 使用有限 poll 直接观察原始事件；随后真正发送数据并关闭 peer
 * 写方向，证明方向隔离不会丢失剩余 payload 或最终读 EOF。
 */
TEST(ReactorContract, LocalWriteShutdownDoesNotInventReadReadiness) {
  descriptor_pair pair;
  auto reactor = faio::io::detail::make_platform_reactor();
  constexpr std::uint64_t generation = 47;
  ASSERT_EQ(reactor.register_resource(pair.fds[0], generation), 0);
  ASSERT_TRUE(wait_flag(reactor, generation, faio::io::detail::writable_bit));
  ASSERT_EQ(::shutdown(pair.fds[0], SHUT_WR), 0);  // 本地读方向仍然开放。
  std::array<readiness_event, 16> events{};
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    const int count = reactor.poll(events, 10);  // 每次等待均有明确上限。
    ASSERT_GE(count, 0);
    for (int index = 0; index < count; ++index) {
      const auto& event = events[static_cast<std::size_t>(index)];
      if (event.key != generation)
        continue;
      EXPECT_EQ(event.flags & faio::io::detail::readable_bit, 0u);
      EXPECT_EQ(event.flags & faio::io::detail::read_closed_bit, 0u)
          << "本地 SHUT_WR 不能表示 peer 已经关闭读侧输入";
    }
  }
  char received{};
  const auto absent = ::recv(pair.fds[0], &received, 1, MSG_DONTWAIT);
  const int absent_error = errno;
  EXPECT_EQ(absent, -1);
  EXPECT_TRUE(absent_error == EAGAIN || absent_error == EWOULDBLOCK);
  const char sent = 'h';
  ASSERT_EQ(::send(pair.fds[1], &sent, 1, 0), 1);
  ASSERT_TRUE(wait_flag(reactor, generation, faio::io::detail::readable_bit));
  ASSERT_EQ(::recv(pair.fds[0], &received, 1, MSG_DONTWAIT), 1);
  EXPECT_EQ(received, sent);
  ASSERT_EQ(::shutdown(pair.fds[1], SHUT_WR), 0);  // peer 写关闭才产生读 EOF。
  EXPECT_TRUE(wait_flag(reactor, generation, faio::io::detail::read_closed_bit));
  EXPECT_EQ(::recv(pair.fds[0], &received, 1, MSG_DONTWAIT), 0);
  reactor.unregister_resource(pair.fds[0]);
}
