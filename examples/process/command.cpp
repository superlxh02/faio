/**
 * @file examples/process/command.cpp
 * @brief 捕获子进程的标准流，并在等待超时后显式终止和回收子进程。
 */
#include <cerrno>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#if defined(FAIO_USE_MODULES) && FAIO_USE_MODULES
import faio;
#else
#include <faio/faio.hpp>
#endif

namespace {
using namespace std::chrono_literals;
using std::chrono::steady_clock;

/**
 * @brief 在子进程中执行父进程指定的操作。
 * @param mode `output` 写入两条标准流，`sleep` 模拟一个耗时命令。
 * @return 指定操作正常结束时返回零。
 * @details 子进程运行当前程序，因此无需查找或安装额外命令。
 */
int run_child(std::string_view mode) {
  if (mode == "output") {
    std::cout << "child output" << std::flush;
    std::cerr << "child diagnostic" << std::flush;
    return std::cout && std::cerr ? 0 : 1;
  }
  if (mode == "sleep") {
    std::this_thread::sleep_for(5s);
    return 0;
  }
  return 2;
}

/**
 * @brief 启动子进程并在一个操作中收集两路输出与退出状态。
 * @param executable 当前示例的绝对路径。
 * @details output 并发读取管道。捕获上限限制输出占用的内存，
 * 截止时间限制整个操作，标准输入设为空设备。
 */
faio::task<void> capture_output(const std::filesystem::path& executable) {
  faio::process::Command command{executable};
  command.args({"--child", "output"}).set_stdin(faio::process::Stdio::null());

  faio::process::CaptureOptions options;
  options.max_stdout_bytes = 1024;
  options.max_stderr_bytes = 1024;
  options.max_total_bytes = 2048;
  options.operation.deadline = steady_clock::now() + 5s;
  auto output = co_await command.output(options);
  if (!output)
    throw std::runtime_error("捕获输出失败：" + std::string{output.error().message()});

  const std::string_view standard_output{output->stdout_data.data(), output->stdout_data.size()};
  const std::string_view standard_error{output->stderr_data.data(), output->stderr_data.size()};
  if (!output->status.success() || standard_output != "child output"
      || standard_error != "child diagnostic")
    throw std::runtime_error("捕获结果与子进程协议不符");
  faio::log::logger()->info("stdout: {}", standard_output);
  faio::log::logger()->info("stderr: {}", standard_error);
}

/**
 * @brief 短暂等待耗时命令，超时后主动终止并回收。
 * @param executable 当前示例的绝对路径。
 * @details wait 到期只结束本次等待。kill 请求终止并等待退出，
 * 随后的 wait 读取已经缓存的退出状态。kill_on_drop 为异常路径提供清理。
 */
faio::task<void> stop_on_timeout(const std::filesystem::path& executable) {
  faio::process::Command command{executable};
  command.args({"--child", "sleep"}).set_stdin(faio::process::Stdio::null()).kill_on_drop(true);
  auto child = co_await command.spawn_async({.deadline = steady_clock::now() + 5s});
  if (!child)
    throw std::runtime_error("启动子进程失败：" + std::string{child.error().message()});

  auto early_status = co_await child->wait({.deadline = steady_clock::now() + 100ms});
  if (early_status || early_status.error().value() != ETIMEDOUT)
    throw std::runtime_error("预期等待子进程时发生超时");

  const auto deadline = steady_clock::now() + 5s;
  auto killed = co_await child->kill({.deadline = deadline});
  if (!killed)
    throw std::runtime_error("终止子进程失败：" + std::string{killed.error().message()});
  auto status = co_await child->wait({.deadline = deadline});
  if (!status)
    throw std::runtime_error("读取退出状态失败：" + std::string{status.error().message()});
  faio::log::logger()->info("等待超时后已终止并回收：{}", status->to_string());
}

/** @brief 在一次异步入口内依次执行捕获和超时演示。 */
faio::task<void> demonstrate(std::filesystem::path executable) {
  co_await capture_output(executable);
  co_await stop_on_timeout(executable);
}
}  // namespace

/**
 * @brief 执行子进程操作，或启动父进程演示。
 * @param argc 直接运行时无需参数。子进程使用 `--child MODE`。
 * @param argv 父进程应当用带目录的程序路径启动，以便找到同一个可执行文件。
 * @return 两项演示成功时返回零。
 */
int main(int argc, char** argv) {
  if (argc == 3 && std::string_view{argv[1]} == "--child")
    return run_child(argv[2]);
  if (argc != 1) {
    faio::log::logger()->error("请直接用可执行文件路径启动此示例。");
    return 2;
  }

  try {
    const auto executable = std::filesystem::canonical(argv[0]);
    faio::block_on(demonstrate(executable));
    return 0;
  } catch (const std::exception& error) {
    faio::log::logger()->error("{}", error.what());
    return 1;
  }
}
