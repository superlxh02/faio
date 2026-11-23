/**
 * @file examples/fs/file_and_directory.cpp
 * @brief 在独立临时目录中演示异步文件写入、读取和目录枚举。
 */
#include <faio/faio.hpp>

#include <exception>
#include <filesystem>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

/** @brief 创建本次运行独占的临时目录。 */
std::filesystem::path create_work_directory() {
  const auto root = std::filesystem::temp_directory_path();
  std::random_device random;
  for (int attempt = 0; attempt < 16; ++attempt) {
    const auto path = root / ("faio-example-" + std::to_string(random()));
    if (std::filesystem::create_directory(path)) {
      return path;
    }
  }
  throw std::runtime_error("无法创建独占的临时目录");
}

/** @brief 写入并重新读取一个文件，随后遍历目录确认条目存在。 */
faio::task<int> demonstrate(const std::filesystem::path& work_directory) {
  constexpr std::string_view message{"hello faio\n"};
  const auto path = work_directory / "message.txt";

  auto file = co_await faio::fs::File::create(path);
  if (!file) {
    faio::log::logger()->error("创建文件失败：{}", file.error().message());
    co_return 1;
  }

  const auto written = co_await file->write_all(std::span<const char>{message});
  if (!written) {
    faio::log::logger()->error("写入文件失败：{}", written.error().message());
    co_return 1;
  }

  const auto closed = co_await file->close();
  if (!closed) {
    faio::log::logger()->error("关闭文件失败：{}", closed.error().message());
    co_return 1;
  }

  const auto contents = co_await faio::fs::read_to_string(path, 1024);
  if (!contents) {
    faio::log::logger()->error("读取文件失败：{}", contents.error().message());
    co_return 1;
  }
  if (*contents != message) {
    faio::log::logger()->error("文件读回内容与写入内容不一致");
    co_return 1;
  }

  auto directory = co_await faio::fs::read_dir(work_directory);
  if (!directory) {
    faio::log::logger()->error("打开目录失败：{}", directory.error().message());
    co_return 1;
  }

  bool found = false;
  for (;;) {
    auto entry = co_await directory->next_entry();
    if (!entry) {
      faio::log::logger()->error("枚举目录失败：{}", entry.error().message());
      co_return 1;
    }
    if (!*entry) {
      break;
    }
    const auto name = (*entry)->file_name();
    faio::log::logger()->info("目录条目：{}", name.string());
    found = found || name == path.filename();
  }

  const auto directory_closed = co_await directory->close();
  if (!directory_closed) {
    faio::log::logger()->error("关闭目录失败：{}", directory_closed.error().message());
    co_return 1;
  }
  if (!found) {
    faio::log::logger()->error("目录中未找到刚写入的文件");
    co_return 1;
  }

  faio::log::logger()->info("文件内容：{}", *contents);
  co_return 0;
}

}  // namespace

/** @brief 等待异步操作结束再清理临时目录。 */
int main() {
  std::filesystem::path directory;
  try {
    directory = create_work_directory();
    const int result = faio::block_on(demonstrate(directory));
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    if (cleanup_error) {
      faio::log::logger()->error("清理临时目录失败：{}", cleanup_error.message());
      return 1;
    }
    return result;
  } catch (const std::exception& error) {
    faio::log::logger()->error("文件与目录示例失败：{}", error.what());
    if (!directory.empty()) {
      std::error_code cleanup_error;
      std::filesystem::remove_all(directory, cleanup_error);
    }
    return 1;
  }
}
