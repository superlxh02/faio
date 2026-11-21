/**
 * @file process_child.cpp
 * @brief 进程单元测试使用的退出与输出程序。
 */
#include <algorithm>
#include <cstdio>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#endif

/** @brief 按参数退出或向两个输出通道写入指定数量的字节。 */
int main(int argc, char** argv) {
  if (argc < 2)
    return 64;
  const std::string mode = argv[1];
  const auto value = argc > 2 ? std::stoull(argv[2]) : 0u;
  if (mode == "exit") {
#if defined(_WIN32)
    ::ExitProcess(static_cast<unsigned>(value));
#else
    return static_cast<int>(value);
#endif
  }
  if (mode == "write-both") {
    const std::string output(1024, 'o');
    const std::string errors(1024, 'e');
    for (std::size_t done = 0; done < value;) {
      const auto count = std::min(output.size(), static_cast<std::size_t>(value) - done);
      if (std::fwrite(output.data(), 1, count, stdout) != count
          || std::fwrite(errors.data(), 1, count, stderr) != count)
        return 74;
      done += count;
    }
    return 0;
  }
  return 64;
}
