/**
 * @file experimental/test_runtime.cpp
 * @brief 实验性运行时选项转换与验证的单元测试。
 */
#include <faio/experimental/runtime_options.h>
#include <gtest/gtest.h>

/** @brief 选项转换保留运行时模式、worker 数量与调度间隔。 */
TEST(ExperimentalRuntime, Options) {
  const faio::experimental::runtime_options options{.mode = faio::runtime::mode::current_thread,
                                                    .workers = 1,
                                                    .io_interval = 13,
                                                    .global_queue_interval = 17,
                                                    .idle_spin_count = 0};
  const auto config = faio::experimental::to_runtime_config(options);
  EXPECT_EQ(config._mode, faio::runtime::mode::current_thread);
  EXPECT_EQ(config._num_workers, 1u);
  EXPECT_EQ(config._io_interval, 13u);
  EXPECT_EQ(config._global_queue_interval, 17u);
  EXPECT_EQ(config._idle_spin_count, 0u);
  EXPECT_THROW(
      faio::experimental::to_runtime_config(faio::experimental::runtime_options{.io_interval = 0}),
      std::invalid_argument);
}
