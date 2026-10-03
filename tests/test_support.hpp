/**
 * @file test_support.hpp
 * @brief 集成测试的结果传播、临时目录与原生句柄 RAII。
 */
#pragma once
#include "faio/faio.hpp"
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace faio_test {
/** @brief 让协程失败通过 block_on 传播，避免 ASSERT 在协程内部提前返回。 */
template <class T>
T take(faio::expected<T> result) {
  if (!result)
    throw std::runtime_error(std::string(result.error().message()));
  return std::move(*result);
}

inline void take(faio::expected<void> result) {
  if (!result)
    throw std::runtime_error(std::string(result.error().message()));
}

/** @brief 每个测试拥有独立目录；测试资源和 runtime 先于目录清理。 */
class temporary_directory {
 public:
  temporary_directory() {
    auto pattern = (std::filesystem::temp_directory_path() / "faio-contract-XXXXXX").string();
    if (!::mkdtemp(pattern.data()))
      throw std::system_error(errno, std::generic_category());
    path_ = std::move(pattern);
  }

  temporary_directory(const temporary_directory&) = delete;

  temporary_directory& operator=(const temporary_directory&) = delete;

  ~temporary_directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

using runtime_context = faio::runtime::detail::runtime_context;
}  // namespace faio_test
