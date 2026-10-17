#include "faio/detail/io/backend/factory.hpp"
#include <gtest/gtest.h>
#include <type_traits>

namespace {
using namespace faio::io::detail;

static_assert(!std::is_polymorphic_v<backend_box>);
static_assert(sizeof(backend_box) == sizeof(std::unique_ptr<backend_box::backend_type>));
#if !defined(_WIN32)
static_assert(!std::is_polymorphic_v<reactor_box>);
static_assert(sizeof(reactor_box) == sizeof(std::unique_ptr<reactor_box::reactor_type>));
#endif

struct state {
  native_registration handle;
  backend_operation cancelled;
  std::uint64_t key{};
  unsigned wakes{}, shutdowns{}, destroyed{};
};

template <bool Native>
struct backend {
  static constexpr bool native_proactor = Native;
  state& observed;

  explicit backend(state& value) : observed(value) {}

  ~backend() { ++observed.destroyed; }

  int attach(int fd, std::uint64_t key) noexcept {
    observed.handle = {static_cast<std::uintptr_t>(fd), native_handle_kind::posix_descriptor};
    observed.key = key;
    return 0;
  }

  void detach(int) noexcept { observed.key = 0; }

  backend_submit_result try_submit(backend_operation) noexcept { return {}; }

  backend_flush_result flush() noexcept { return {}; }

  int poll(std::span<backend_event>, std::optional<int>) noexcept { return 0; }

  void request_cancel(std::uint64_t token) noexcept { observed.cancelled.token = token; }

  void wake() noexcept { ++observed.wakes; }

  void begin_shutdown() noexcept { ++observed.shutdowns; }

  bool quiescent() noexcept { return true; }

  bool supports(std::uint32_t) noexcept { return false; }

  const char* name() const noexcept { return "test-static"; }

  backend_statistics statistics() const noexcept { return {}; }
};

struct typed_backend : backend<true> {
  using backend::backend;
  static constexpr bool poll_flushes_submissions = true;

  int attach(native_registration handle, std::uint64_t key) noexcept {
    observed.handle = handle;
    observed.key = key;
    return 0;
  }

  void detach(native_registration handle) noexcept { observed.handle = handle; }

  void request_cancel(backend_operation op) noexcept { observed.cancelled = op; }
};

TEST(StaticIODispatch, PosixAdapterRejectsWrongKindAndNarrowingBeforeCallingBackend) {
  state observed;
  basic_backend_box<backend<false>> box{std::make_unique<backend<false>>(observed)};
  EXPECT_EQ(box.attach({42, native_handle_kind::windows_socket}, 7), -22);
  EXPECT_EQ(box.attach({0x80000000U, native_handle_kind::posix_descriptor}, 7), -22);
  EXPECT_EQ(observed.key, 0u);
  EXPECT_EQ(box.attach(42, 7), 0);
  EXPECT_EQ(observed.key, 7u);
  box.detach({42, native_handle_kind::windows_handle});
  EXPECT_EQ(observed.key, 7u);
  box.detach(42);
  EXPECT_EQ(observed.key, 0u);
}

TEST(StaticIODispatch, TypedHandlesAndCancellationPreserveFullIdentity) {
  state observed;
  basic_backend_box<typed_backend> box{std::make_unique<typed_backend>(observed)};
  const native_registration handle{~std::uintptr_t{}, native_handle_kind::windows_handle};
  EXPECT_EQ(box.attach(handle, 9), 0);
  EXPECT_EQ(observed.handle.value, handle.value);
  EXPECT_EQ(observed.handle.kind, handle.kind);
  int payload{};
  const backend_operation op{0xfedcba9876543210ULL, &payload};
  box.request_cancel(op);
  EXPECT_EQ(observed.cancelled.token, op.token);
  EXPECT_EQ(observed.cancelled.request, &payload);
  EXPECT_TRUE(box.poll_flushes_submissions());
}

TEST(StaticIODispatch, OptionalFailurePolicyKeepsNativeBorrowBoundaryConservative) {
  state native, readiness;
  basic_backend_box<backend<true>> n{std::make_unique<backend<true>>(native)};
  basic_backend_box<backend<false>> r{std::make_unique<backend<false>>(readiness)};
  EXPECT_FALSE(n.begin_failure(5));
  EXPECT_TRUE(r.begin_failure(5));
  EXPECT_EQ(native.shutdowns, 1u);
  EXPECT_EQ(readiness.shutdowns, 1u);
  EXPECT_FALSE(n.poll_flushes_submissions());
  r.request_cancel({0xabcdef1234567890ULL, nullptr});
  EXPECT_EQ(readiness.cancelled.token, 0xabcdef1234567890ULL);
}

TEST(StaticIODispatch, MoveKeepsSingleOwnerAndConstWakeReachesConcreteObject) {
  state observed;
  {
    basic_backend_box<backend<false>> first{std::make_unique<backend<false>>(observed)};
    auto moved = std::move(first);
    const auto& view = moved;
    view.wake();
    EXPECT_EQ(observed.wakes, 1u);
    EXPECT_STREQ(view.name(), "test-static");
    EXPECT_EQ(observed.destroyed, 0u);
  }
  EXPECT_EQ(observed.destroyed, 1u);
}
}  // namespace
