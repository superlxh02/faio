/**
 * @file experimental/test_reflection.cpp
 * @brief 反射注解读取运行时选项的单元测试。
 */
#include <faio/experimental/reflection.h>
#include <gtest/gtest.h>

/** @brief 携带运行时选项的测试类型。 */
struct[[= faio::experimental::runtime_options{.workers = 3, .io_interval = 7}]] configured_type {};

/** @brief 类型注解提供声明的运行时配置。 */
TEST(ExperimentalReflection, AnnotatedOptions) {
  constexpr auto options = faio::experimental::runtime_options_of<^^configured_type>();
  EXPECT_EQ(options.workers, 3u);
  EXPECT_EQ(options.io_interval, 7u);
}
