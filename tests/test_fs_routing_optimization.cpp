#include "backend_test_support.hpp"
#include "faio/detail/fs/provider/file_request.hpp"
#include <array>
#include <atomic>
#include <gtest/gtest.h>

namespace {
struct observations {
  unsigned native_builds{};
  std::atomic<unsigned> fallback_calls{};
  bool vector_storage_preserved{}, path_storage_preserved{};
};

/** 参数拥有一次；路由工厂的计数不涉及实际文件系统调用。 */
struct observed_operation {
  observations* observed;
  faio::io::detail::operation_kind operation;
  std::vector<iovec> vectors{8};
  std::string path = std::string(256, 'p');

  auto kind() const noexcept { return operation; }

  faio::io::detail::io_request into_request() && {
    ++observed->native_builds;
    const auto* vector_data = vectors.data();
    const auto* path_data = path.data();
    faio::io::detail::io_request request;
    request.kind = kind();
    request.vectors = std::move(vectors);
    request.path = std::move(path);
    observed->vector_storage_preserved = request.vectors.data() == vector_data;
    observed->path_storage_preserved = request.path.data() == path_data;
    return request;
  }

  faio::expected<std::size_t> operator()() const noexcept {
    observed->fallback_calls.fetch_add(1, std::memory_order_relaxed);
    return vectors.size() + path.size();
  }
};

faio::task<bool> routing_constructs_only_selected_parameters() {
  auto context = faio::io::io_context::current();
  const auto kind = faio::io::detail::operation_kind::read;
  const bool native = faio::io::supports_native(context, kind);
  observations observed;
  {
    // 未 await 不得执行 syscall；原生分支可以在此接管请求参数。
    auto operation =
        faio::fs::detail::execute_file_request(context, observed_operation{&observed, kind});
    (void)operation;
  }
  co_return observed.native_builds == static_cast<unsigned>(native)
      && !observed.fallback_calls.load(std::memory_order_relaxed)
      && (!native || (observed.vector_storage_preserved && observed.path_storage_preserved));
}

faio::task<bool> fallback_uses_owned_parameters_once() {
  observations observed;
  auto operation = faio::fs::detail::execute_file_request(
      faio::io::io_context::current(),
      observed_operation{&observed, faio::io::detail::operation_kind::unsupported});
  if (observed.native_builds || observed.fallback_calls.load(std::memory_order_relaxed))
    co_return false;
  const auto result = co_await operation;
  co_return result && *result == 264 && !observed.native_builds
      && observed.fallback_calls.load(std::memory_order_relaxed) == 1;
}

TEST(FilesystemRoutingOptimization, ConstructsOnlySelectedBranchAndNativeMovesOwnedStorage) {
  faio::runtime::detail::runtime_context runtime{
      faio_test::config_builder().set_mode(faio::runtime::mode::current_thread).build()};
  EXPECT_TRUE(runtime.block_on(routing_constructs_only_selected_parameters()));
}

TEST(FilesystemRoutingOptimization, FallbackHasNoNativeRequestCopyAndRunsOnce) {
  faio::runtime::detail::runtime_context runtime{
      faio_test::config_builder().set_mode(faio::runtime::mode::current_thread).build()};
  EXPECT_TRUE(runtime.block_on(fallback_uses_owned_parameters_once()));
}

TEST(FilesystemRoutingOptimization, EmptyAndInvalidBlocksAreImmediateWithoutServiceState) {
  // 校验和空操作不能解引用空 File 状态，也不能构造执行器任务。
  std::shared_ptr<faio::fs::detail::file_state> absent;
  std::array<char, 1> byte{};
  auto empty = faio::fs::detail::read_block(absent, {}, 0);
  EXPECT_TRUE(empty.await_ready());
  const auto empty_result = empty.await_resume();
  ASSERT_TRUE(empty_result);
  EXPECT_EQ(*empty_result, 0u);
  auto invalid = faio::fs::detail::read_block(absent, byte, UINT64_MAX);
  EXPECT_TRUE(invalid.await_ready());
  const auto result = invalid.await_resume();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().value(), EOVERFLOW);
}
}  // namespace
