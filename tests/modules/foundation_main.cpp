#include <cerrno>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#if defined(FAIO_USE_HEADERS) && FAIO_USE_HEADERS
#include <faio/common/error.hpp>
#else
import faio;
#endif

faio::expected<int> peer_result(bool fail);
void peer_cancel();

static_assert(std::is_same_v<faio::expected<int>, std::expected<int, faio::error>>);
static_assert(static_cast<int>(faio::error_domain::PROCESS) == 7);

int main() {
  const auto success = peer_result(false);
  const auto failure = peer_result(true);
  if (!success || *success != 42 || failure) {
    std::fputs("unexpected cross-TU result\n", stderr);
    return 1;
  }

  const auto& error = failure.error();
  const auto updated = error.with_progress(12);
  if (error.transferred() != 7 || updated.progress() != 12 || updated.native_code() != 3
      || updated.domain() != faio::error_domain::PROCESS || !updated.context()
      || updated.context() != error.context()) {
    std::fputs("error identity or progress changed across TUs\n", stderr);
    return 1;
  }
  const auto& context = *updated.context();
  if (context.process_exit_code != 17 || context.captured_stdout != std::vector<char>{'x', '\0'}
      || !context.cause || context.cause->message() != "Write return zero"
      || std::format("{}", updated) != "child failed (error 3)") {
    std::fputs("error context or formatting changed across TUs\n", stderr);
    return 1;
  }

  const auto native = faio::make_error(EINVAL);
  const faio::expected<void> empty;
  if (!empty || native.domain() != faio::error_domain::POSIX || native.progress() != 0
      || native.context() || native.message() != std::strerror(EINVAL)
      || faio::error{3, 0, faio::error_domain::TLS}.message() != "TLS error"
      || faio::error{3, 0, faio::error_domain::PROCESS}.message() != "Process error") {
    std::fputs("native or component error lookup changed\n", stderr);
    return 1;
  }

  try {
    peer_cancel();
  } catch (const faio::operation_cancelled& cancelled) {
    if (std::string_view{cancelled.what()} == "协程等待操作已取消")
      return 0;
  }
  std::fputs("cross-TU cancellation failed\n", stderr);
  return 1;
}
