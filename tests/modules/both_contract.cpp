#include <chrono>
#include <optional>
#include <stop_token>
#include <type_traits>

#if defined(FAIO_USE_HEADERS) && FAIO_USE_HEADERS
#include <faio/tls/handshake.hpp>
#include <faio/process/command.hpp>
#else
import faio;
import faio.tls;
import faio.process;
#endif

static_assert(
    std::is_same_v<decltype(faio::process::CaptureOptions::operation), faio::io::OperationOptions>);

int main() {
  std::stop_source stop;
  faio::io::OperationOptions operation{.stop = stop.get_token()};
  faio::process::CaptureOptions capture;
  capture.operation = operation;
  const auto name = faio::tls::ServerName::parse("localhost");
  stop.request_stop();
  return name && capture.operation.stop.stop_requested() ? 0 : 1;
}
