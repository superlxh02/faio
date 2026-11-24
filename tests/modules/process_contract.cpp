#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(FAIO_USE_HEADERS) && FAIO_USE_HEADERS
#include <faio/faio.hpp>
#else
import faio;
#endif

namespace {
faio::task<bool> capture(const std::filesystem::path& path) {
  using namespace std::chrono_literals;
  faio::process::Command command{path};
  command.args({"--child"});
  faio::process::CaptureOptions options;
  options.operation.deadline = std::chrono::steady_clock::now() + 5s;
  const auto result = co_await command.output(options);
  if (!result) {
    std::fprintf(stderr, "binary capture failed with error %d\n", result.error().value());
    co_return false;
  }
  if (result->status.code() != 7 || result->stdout_data != std::vector<char>{'a', '\0', 'b'}
      || result->stderr_data != std::vector<char>{'x', '\0', 'y'}) {
    std::fprintf(stderr, "binary capture returned status %d, stdout %zu bytes and stderr %zu bytes\n",
                 static_cast<int>(result->status.code().value_or(255)), result->stdout_data.size(),
                 result->stderr_data.size());
    co_return false;
  }
  faio::process::Command limited_command{path};
  limited_command.arg("--child-limited");
  options.operation.deadline = std::chrono::steady_clock::now() + 5s;
  options.max_stdout_bytes = 1;
  const auto limited = co_await limited_command.output(options);
  if (limited || limited.error().domain() != faio::error_domain::process
      || limited.error().value() != static_cast<int>(faio::process::errc::output_limit_exceeded)) {
    std::fprintf(stderr, "limited capture returned status or error %d\n",
                 limited ? static_cast<int>(limited->status.code().value_or(255)) : limited.error().value());
    co_return false;
  }
  const auto& diagnostics = limited.error().context();
  const bool matches = diagnostics && diagnostics->process_id.has_value() && diagnostics->stdout_truncated
      && diagnostics->captured_stdout == std::vector<char>{'a'}
      && limited.error().progress() == diagnostics->captured_stdout.size()
                                      + diagnostics->captured_stderr.size();
  if (!matches)
    std::fprintf(stderr, "limited capture returned inconsistent truncation diagnostics\n");
  co_return matches;
}
}  // namespace

int main(int argc, char** argv) {
  const auto mode = argc == 2 ? std::string_view{argv[1]} : std::string_view{};
  if (mode == "--child" || mode == "--child-limited") {
    if (std::fwrite("a\0b", 1, 3, stdout) != 3 || std::fwrite("x\0y", 1, 3, stderr) != 3)
      return 8;
    if (mode == "--child-limited") {
      if (std::fflush(stdout) != 0 || std::fflush(stderr) != 0)
        return 8;
      // Keep the child alive until output capture detects the limit and requests termination.
      std::this_thread::sleep_for(std::chrono::seconds{10});
    }
    return 7;
  }
  return faio::block_on(capture(std::filesystem::canonical(argv[0]))) ? 0 : 1;
}
