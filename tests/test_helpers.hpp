/**
 * @file test_helpers.hpp
 * @brief 单元测试的运行时配置、结果检查与临时目录。
 */
#pragma once

#include <faio/faio.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace faio_test {
/** @brief 创建使用当前构建后端、包含两个 worker 的测试配置。 */
inline auto config(faio::runtime::mode mode = faio::runtime::mode::multi_thread) {
  return faio::ConfigBuilder{}.set_mode(mode).set_num_workers(2).build();
}

/** @brief 领取成功结果，将错误传播为测试异常。 */
template <class T>
T take(faio::expected<T> result) {
  if (!result)
    throw std::runtime_error(std::string(result.error().message()));
  return std::move(*result);
}

/** @brief 检查无返回值操作是否成功。 */
inline void take(faio::expected<void> result) {
  if (!result)
    throw std::runtime_error(std::string(result.error().message()));
}

/** @brief 持有测试目录的所有权，并在作用域结束时清理其内容。 */
class temporary_directory {
 public:
  /** @brief 在系统临时目录中创建独立目录。 */
  temporary_directory() {
    static std::atomic<unsigned> sequence{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt != 16; ++attempt) {
      path_ =
          std::filesystem::temp_directory_path()
          / ("faio-unit-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1)));
      if (std::filesystem::create_directory(path_))
        return;
    }
    throw std::runtime_error("无法创建测试临时目录");
  }

  /** @brief 临时目录所有权不可复制。 */
  temporary_directory(const temporary_directory&) = delete;
  /** @brief 临时目录所有权不可复制赋值。 */
  temporary_directory& operator=(const temporary_directory&) = delete;

  /** @brief 清理目录，不从析构函数传播文件系统错误。 */
  ~temporary_directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /** @brief 返回目录路径。 */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_; /**< 测试拥有的目录路径。 */
};
}  // namespace faio_test
