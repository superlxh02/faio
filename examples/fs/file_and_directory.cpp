/**
 * @file examples/fs/file_and_directory.cpp
 * @brief 在独立临时目录中演示异步文件写入、读取和目录枚举。
 */
#include <faio/faio.hpp>
#include <exception>
#include <filesystem>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {

/**
 * @brief 创建本次运行独占的临时目录。
 * @return 新建目录的路径。调用方负责清理此目录及其中的文件。
 */
std::filesystem::path create_work_directory() {
  const auto root = std::filesystem::temp_directory_path();
  std::random_device random;
  for (int attempt = 0; attempt < 16; ++attempt) {
    const auto path = root / ("faio-example-" + std::to_string(random()));
    /** @brief create_directory 返回 true 才取得目录。已有同名目录时尝试新的名称。 */
    if (std::filesystem::create_directory(path))
      return path;
  }
  throw std::runtime_error("无法创建独占的临时目录");
}

/**
 * @brief 检查异步操作结果，保留失败环节和文件系统错误信息。
 * @tparam T 成功结果的类型，可以是 void。
 * @param result 等待文件系统操作所得的结果。
 * @param step 出错时打印的调用环节。
 * @return 成功结果的值。
 */
template <class T>
T checked(faio::expected<T> result, std::string_view step) {
  if (!result)
    throw std::runtime_error(std::string{step} + "失败：" + std::string{result.error().message()});
  if constexpr (!std::is_void_v<T>)
    return std::move(*result);
}

/**
 * @brief 写入并重新读取一个文件，随后遍历目录确认条目存在。
 * @param work_directory 已创建的空目录，操作期间保持有效。
 */
faio::task<void> demonstrate(const std::filesystem::path& work_directory) {
  constexpr std::string_view message{"hello faio\n"};
  const auto path = work_directory / "message.txt";
  auto file = checked(co_await faio::fs::File::create(path), "创建文件");
  /** @brief 写入时借用 message 的字节。等待文件关闭后，再用路径接口重新打开并读取。 */
  checked(co_await file.write_all(std::span<const char>{message}), "写入文件");
  checked(co_await file.close(), "关闭文件");
  const auto contents = checked(co_await faio::fs::read_to_string(path, 1024), "读取文件");
  if (contents != message)
    throw std::runtime_error("文件读回内容与写入内容不一致");

  auto directory = checked(co_await faio::fs::read_dir(work_directory), "打开目录");
  bool found = false;
  /** @brief checked 先处理外层 expected。内层 optional 为空才表示枚举正常结束。 */
  while (auto entry = checked(co_await directory.next_entry(), "枚举目录")) {
    const auto name = entry->file_name();
    std::cout << "目录条目：" << name.string() << '\n';
    found = found || name == path.filename();
  }
  checked(co_await directory.close(), "关闭目录");
  if (!found)
    throw std::runtime_error("目录中未找到刚写入的文件");
  std::cout << "文件内容：" << contents;
}

}  // namespace

/** @brief 等待异步操作结束再清理临时目录，并在清理失败时保留原错误。 */
int main() {
  try {
    const auto directory = create_work_directory();
    std::exception_ptr operation_error;
    try {
      faio::block_on(demonstrate(directory));
    } catch (...) {
      operation_error = std::current_exception();
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    if (cleanup_error)
      std::cerr << "清理临时目录失败：" << cleanup_error.message() << '\n';
    if (operation_error)
      std::rethrow_exception(operation_error);
    return cleanup_error ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << "文件与目录示例失败：" << error.what() << '\n';
    return 1;
  }
}
