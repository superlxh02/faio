/**
 * @file test_backend_selection.cpp
 * @brief 默认、显式与构建能力共同决定后端；模拟旧内核不依赖测试机器版本。
 * @details 行为矩阵复用同一 binary，选择错误必须在创建 IO 服务时明确失败。
 */
#include "backend_test_support.hpp"
#include <array>
#include <gtest/gtest.h>
#include <memory>
#include <string_view>

#if defined(__linux__)
#include "faio/detail/io/backend_selection.hpp"
#if FAIO_HAS_IO_URING
#include <liburing.h>
#endif

TEST(BackendSelection, KernelVersionUsesNumericMajorAndMinor) {
  using faio::io::detail::parse_kernel_version;
  const auto stable = parse_kernel_version("5.10.237-distribution");
  EXPECT_EQ(stable.major, 5u);
  EXPECT_EQ(stable.minor, 10u);
  const auto future = parse_kernel_version("7.0.14-orbstack");
  EXPECT_EQ(future.major, 7u);
  EXPECT_EQ(future.minor, 0u);
  EXPECT_THROW((void)parse_kernel_version("bad.10"), std::invalid_argument);
  EXPECT_THROW((void)parse_kernel_version("5"), std::invalid_argument);
  EXPECT_THROW((void)parse_kernel_version("5.bad"), std::invalid_argument);
}

TEST(BackendSelection, OldKernelUsesEpollAndRejectsExplicitUring) {
  using faio::io::detail::resolve_io_backend;
  using faio::runtime::io_backend;
  EXPECT_EQ(resolve_io_backend(std::nullopt, {4, 19}), io_backend::IO_EPOLL);
  EXPECT_EQ(resolve_io_backend(std::nullopt, {5, 9}), io_backend::IO_EPOLL);
  EXPECT_EQ(resolve_io_backend(io_backend::IO_EPOLL, {5, 4}), io_backend::IO_EPOLL);
  EXPECT_THROW((void)resolve_io_backend(io_backend::IO_URING, {5, 9}), std::invalid_argument);
}

TEST(BackendSelection, NativeBuildDefaultsToUringOnSupportedKernel) {
  using faio::io::detail::resolve_io_backend;
  using faio::runtime::io_backend;
#if FAIO_HAS_IO_URING
  EXPECT_EQ(resolve_io_backend(std::nullopt, {5, 10}), io_backend::IO_URING);
  EXPECT_EQ(resolve_io_backend(std::nullopt, {6, 0}), io_backend::IO_URING);
  EXPECT_EQ(resolve_io_backend(std::nullopt, {7, 0}), io_backend::IO_URING);
  EXPECT_EQ(resolve_io_backend(io_backend::IO_URING, {5, 10}), io_backend::IO_URING);
#else
  EXPECT_EQ(resolve_io_backend(std::nullopt, {5, 10}), io_backend::IO_EPOLL);
  EXPECT_EQ(resolve_io_backend(std::nullopt, {7, 0}), io_backend::IO_EPOLL);
  EXPECT_THROW((void)resolve_io_backend(io_backend::IO_URING, {7, 0}), std::invalid_argument);
  // OFF构建需要在真正创建engine时拒绝明确uring选择，不能只验证纯选择函数。
  faio::io::engine_config unavailable{};
  unavailable.requested_backend = io_backend::IO_URING;
  EXPECT_THROW((void)faio::io::io_engine{unavailable}, std::invalid_argument);
#endif
  EXPECT_EQ(resolve_io_backend(io_backend::IO_EPOLL, {7, 0}), io_backend::IO_EPOLL);
}

#if FAIO_HAS_IO_URING
/**
 * @brief 内核支持的文件/路径 opcode 不可被封装层错误地报告为 provider。
 * @details 独立查询内核 probe，与明确创建的 uring domain 能力对照；旧内核
 * 缺少 opcode 时双方应一致，不用伪造的版本字符串代替实际内核能力。
 */
TEST(BackendSelection, NativeFilesystemOpcodeCapabilitiesMatchActualKernelProbe) {
  using faio::io::detail::operation_kind;
  if (faio_test::requested_backend() == faio::runtime::io_backend::IO_EPOLL)
    GTEST_SKIP() << "原生 opcode probe 由 uring 矩阵验证；epoll 不要求原生队列权限";
  if (faio::io::detail::resolve_io_backend(faio_test::requested_backend())
      != faio::runtime::io_backend::IO_URING)
    GTEST_SKIP() << "当前内核默认使用 epoll，无 uring probe 要求";
  std::unique_ptr<io_uring_probe, decltype(&::io_uring_free_probe)> probe{::io_uring_get_probe(),
                                                                          &::io_uring_free_probe};
  ASSERT_NE(probe, nullptr);
  auto config = faio_test::engine_config();
  config.requested_backend = faio::runtime::io_backend::IO_URING;
  faio::io::io_engine engine{config};
  const std::array mapping{std::pair{operation_kind::statx, IORING_OP_STATX},
                           std::pair{operation_kind::mkdirat, IORING_OP_MKDIRAT},
                           std::pair{operation_kind::unlinkat, IORING_OP_UNLINKAT},
                           std::pair{operation_kind::renameat, IORING_OP_RENAMEAT},
                           std::pair{operation_kind::linkat, IORING_OP_LINKAT},
                           std::pair{operation_kind::symlinkat, IORING_OP_SYMLINKAT},
                           std::pair{operation_kind::ftruncate, IORING_OP_FTRUNCATE}};
  for (const auto& [kind, opcode] : mapping) {
    const bool kernel_supported = ::io_uring_opcode_supported(probe.get(), opcode);
    EXPECT_EQ(engine.context().domain()->supports_native(kind), kernel_supported)
        << "filesystem opcode=" << static_cast<unsigned>(opcode);
  }
}
#endif
#endif

/** @brief 名称与 native_filesystem 能力同时匹配真实选择，避免仅改字符串。 */
TEST(BackendSelection, EngineCapabilitiesMatchExplicitMatrixChoice) {
  faio::io::io_engine engine{faio_test::engine_config()};
  const auto capabilities = engine.capabilities();
  EXPECT_TRUE(capabilities.network);
  EXPECT_TRUE(capabilities.filesystem);
  EXPECT_TRUE(capabilities.vectored);
  EXPECT_TRUE(capabilities.readiness);
#if defined(__linux__)
  const auto requested = faio_test::requested_backend();
  const auto resolved = faio::io::detail::resolve_io_backend(requested);
  const bool native = resolved == faio::runtime::io_backend::IO_URING;
  EXPECT_EQ(std::string_view{capabilities.backend}, native ? "uring" : "epoll");
  EXPECT_EQ(capabilities.native_filesystem, native);
#else
  EXPECT_EQ(std::string_view{capabilities.backend}, "kqueue");
  EXPECT_FALSE(capabilities.native_filesystem);
#endif
}
