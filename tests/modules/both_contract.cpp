#include <chrono>
#include <optional>
#include <stop_token>
#include <type_traits>

#if defined(FAIO_USE_HEADERS) && FAIO_USE_HEADERS
#include <faio/faio.hpp>
#else
import faio;
#endif

static_assert(std::is_same_v<decltype(faio::process::capture_options::operation),
                             faio::io::operation_options>);

int main() {
  std::stop_source stop;
  faio::io::operation_options operation{.stop = stop.get_token()};
  faio::process::capture_options capture;
  capture.operation = operation;
  const auto name = faio::tls::server_name::parse("localhost");
  stop.request_stop();
  return name && capture.operation.stop.stop_requested() ? 0 : 1;
}
