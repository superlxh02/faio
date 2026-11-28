#include <expected>
#include <memory>

#if defined(FAIO_USE_HEADERS) && FAIO_USE_HEADERS
#include <faio/common/error.hpp>
#else
import faio;
#endif

faio::expected<int> peer_result(bool fail) {
  if (!fail)
    return 42;
  auto context = std::make_shared<faio::error_context>();
  context->message = "child failed";
  context->cause = faio::make_error(faio::error::WRITE_ZERO);
  context->captured_stdout = {'x', '\0'};
  context->process_exit_code = 17;
  return std::unexpected{faio::error{3, 7, faio::error_domain::PROCESS}.with_context(context)};
}

void peer_cancel() {
  throw faio::operation_cancelled{};
}
