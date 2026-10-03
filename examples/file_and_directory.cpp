/**
 * 文件与目录操作：在启动程序时的工作目录中生成 test/main.c。
 * 完整流程：创建目录 -> 创建并写入 C 源码 -> 展示 test 目录 ->
 * 进入 test -> 枚举其中的所有条目 -> 读取并打印 main.c。
 *
 * 本示例只有一个 cpp，不依赖其他 example。文件与目录操作使用 faio::fs
 * 的可等待接口；改变进程工作目录使用标准库的 current_path。
 * 重复运行时保留 test 中的其他条目，但会覆盖 main.c，运行结束不删除文件。
 */
#include "faio/faio.hpp"
#include "faio/log.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
faio::task<void> example_file_and_directory() {
  // 工作目录指从哪个目录启动程序，与可执行文件所在的目录没有必然关系。
  // 先保存绝对路径，之后即使进入 test，也能准确恢复原来的位置。
  const auto initial_directory = std::filesystem::current_path();
  const auto test_directory = initial_directory / "test";
  const auto source_path = test_directory / "main.c";
  std::cout << "运行目录：" << initial_directory.string() << '\n';

  // 1. 异步创建目录。create_dir_all 在目录已存在时也能成功，因此可重复运行。
  //    如果 test 已经是普通文件，则返回错误，不能继续把它当作目录使用。
  auto created = co_await faio::fs::create_dir_all(test_directory);
  if (!created)
    throw std::runtime_error("创建 test 目录失败：" +
                             std::string{created.error().message()});
  std::cout << "1. 已创建目录：" << test_directory.string() << '\n';

  // 原始字符串中的内容就是写入 main.c 的 C 源码。
  // printf 的 \n 保留在源码里，由 C 程序运行时解释为换行。
  constexpr std::string_view source = R"(#include <stdio.h>

int main(void) {
    printf("Hello, world!\n");
    return 0;
}
)";

  // 2. File::create 创建一个可写文件，已有文件会被截断后重新写入。
  //    write_all 会处理短写，直到所有字节写完或返回错误；不是只写一次。
  {
    auto opened = co_await faio::fs::File::create(source_path);
    if (!opened)
      throw std::runtime_error("创建 main.c 失败：" +
                               std::string{opened.error().message()});
    auto file = std::move(*opened);
    auto written = co_await file.write_all(std::span<const char>{source});
    if (!written)
      throw std::runtime_error("写入 main.c 失败：" +
                               std::string{written.error().message()});
    // 明确等待关闭，再进行后面的目录操作和读取，便于看清资源的生命周期。
    auto closed = co_await file.close();
    if (!closed)
      throw std::runtime_error("关闭 main.c 失败：" +
                               std::string{closed.error().message()});
  }
  std::cout << "2. 已写入 C 源码：" << source_path.string() << '\n';

  // 3. 打开运行目录，逐个枚举条目，可以在输出中看到新建的 [目录] test/。
  //    read_dir 返回 ReadDir；next_entry 每次取得一个条目，不一次加载整个目录。
  {
    auto opened = co_await faio::fs::read_dir(initial_directory);
    if (!opened)
      throw std::runtime_error("打开运行目录失败：" +
                               std::string{opened.error().message()});
    auto directory = std::move(*opened);
    std::cout << "3. 运行目录中的条目：\n";
    for (;;) {
      auto entry = co_await directory.next_entry();
      // 外层 expected 表示操作是否成功；内层 optional 为空才表示枚举结束。
      if (!entry)
        throw std::runtime_error("枚举运行目录失败：" +
                                 std::string{entry.error().message()});
      if (!*entry)
        break;
      auto type = co_await (*entry)->file_type();
      if (!type)
        throw std::runtime_error("获取条目类型失败：" +
                                 std::string{type.error().message()});
      const bool is_directory = *type == std::filesystem::file_type::directory;
      std::cout << (is_directory ? "  [目录] " : "  [条目] ")
                << (*entry)->file_name().string() << (is_directory ? "/" : "")
                << '\n';
    }
    auto closed = co_await directory.close();
    if (!closed)
      throw std::runtime_error("关闭运行目录失败：" +
                               std::string{closed.error().message()});
  }

  // 4. 真正改变当前进程的工作目录，后面使用相对路径 "." 和 "main.c"。
  //    faio 没有 chdir 接口，这一步用标准库。工作目录是整个进程共享的状态，
  //    所以本例按顺序执行，前面的操作都已完成，也没有并发的相对路径任务。
  std::filesystem::current_path(test_directory);
  try {
    std::cout << "4. 已进入目录：" << std::filesystem::current_path().string()
              << '\n';

    // 5. 独立写出 test 内的枚举过程，展示其中所有条目，而不只查找 main.c。
    //    ReadDir 自动跳过 "." 和 ".."；枚举顺序由文件系统决定，无需排序。
    {
      auto opened = co_await faio::fs::read_dir(".");
      if (!opened)
        throw std::runtime_error("打开 test 目录失败：" +
                                 std::string{opened.error().message()});
      auto directory = std::move(*opened);
      std::cout << "5. test 目录中的所有条目：\n";
      for (;;) {
        auto entry = co_await directory.next_entry();
        if (!entry)
          throw std::runtime_error("枚举 test 目录失败：" +
                                   std::string{entry.error().message()});
        if (!*entry)
          break;
        auto type = co_await (*entry)->file_type();
        if (!type)
          throw std::runtime_error("获取条目类型失败：" +
                                   std::string{type.error().message()});
        const bool is_directory =
            *type == std::filesystem::file_type::directory;
        std::cout << (is_directory ? "  [目录] " : "  [条目] ")
                  << (*entry)->file_name().string() << (is_directory ? "/" : "")
                  << '\n';
      }
      auto closed = co_await directory.close();
      if (!closed)
        throw std::runtime_error("关闭 test 目录失败：" +
                                 std::string{closed.error().message()});
    }

    // 6. 现在相对路径 main.c 就是 test/main.c。read_to_string 封装了打开、
    //    循环读取和资源释放；这里限制为 4096 字节，足够容纳本例的小段源码。
    auto contents = co_await faio::fs::read_to_string("main.c", 4096);
    if (!contents)
      throw std::runtime_error("读取 main.c 失败：" +
                               std::string{contents.error().message()});
    if (*contents != source)
      throw std::runtime_error("main.c 读回内容与写入内容不一致");
    std::cout << "6. main.c 的内容：\n" << *contents;
  } catch (...) {
    // 发生异常也尝试恢复目录；保留原始读写错误，交给 main 输出。
    std::error_code restore_error;
    std::filesystem::current_path(initial_directory, restore_error);
    throw;
  }

  std::filesystem::current_path(initial_directory);
  std::cout << "已恢复运行目录：" << initial_directory.string() << '\n';
}
} // namespace

int main(int argc, char **argv) {
  try {
    // 后端参数完整写在本文件中，与其他 example 没有源码复用关系。
    auto builder = faio::config_builder{};
#if defined(__linux__)
    if (argc > 2)
      throw std::invalid_argument("仅支持 --io-backend=epoll|uring");
    std::string_view selection;
    if (argc == 2) {
      constexpr std::string_view prefix{"--io-backend="};
      const std::string_view argument{argv[1]};
      if (!argument.starts_with(prefix) || argument.size() == prefix.size())
        throw std::invalid_argument("请使用 --io-backend=epoll|uring");
      selection = argument.substr(prefix.size());
    } else if (const char *environment = std::getenv("FAIO_TEST_IO_BACKEND")) {
      selection = environment;
    }
    if (selection == "epoll")
      builder.set_io_backend(faio::runtime::io_backend::IO_EPOLL);
    else if (selection == "uring")
      builder.set_io_backend(faio::runtime::io_backend::IO_URING);
    else if (!selection.empty())
      throw std::invalid_argument("IO 后端必须是 epoll 或 uring");
#else
    (void)argv;
    if (argc > 1)
      throw std::invalid_argument(
          "本平台使用固定 IO 后端，无需选择 Linux 后端");
#endif
    faio::runtime::configure(builder.set_num_workers(2).build());
    faio::block_on(example_file_and_directory());
    faio::runtime::shutdown();
  } catch (const std::exception &error) {
    faio::log::logger()->error("文件与目录示例失败：{}", error.what());
    return 1;
  }
}
