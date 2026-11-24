#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#if defined(unix)
#undef unix
#endif
#endif

#if defined(FAIO_EXPERIMENTAL_HAS_REFLECTION) && FAIO_EXPERIMENTAL_HAS_REFLECTION
#include <meta>
#endif

#if defined(FAIO_USE_HEADERS) && FAIO_USE_HEADERS
#include <faio/faio.hpp>
#else
import faio;
#endif

namespace {

struct immediate_value {
  template <class Completion>
  void on_suspend(Completion completion) noexcept {
    std::move(completion).complete();
  }

  int on_resume() && noexcept { return 42; }
};

faio::task<void> stable_contract() {
  using namespace std::chrono_literals;

  co_await faio::time::sleep(1ms);
  auto endpoints = faio::sync::mpsc<int>::make(1);
  (co_await endpoints.first.send(co_await faio::make_awaiter(immediate_value{}))).value();
  if ((co_await endpoints.second.recv()).value() != 42)
    throw std::runtime_error("channel lost the awaiter result");

  const std::array<char, 4> payload{'f', 'a', 'i', 'o'};
  std::array<char, 4> bytes{};
  faio::io::MemoryStream stream;
  (co_await faio::io::write_all(stream, payload)).value();
  const auto offset = (co_await stream.seek(0)).value();
  const auto read = (co_await faio::io::read_exact(stream, bytes)).value();
  if (offset != 0 || read != payload.size() || bytes != payload)
    throw std::runtime_error("stream round trip failed");

  auto [left, right] = faio::io::duplex(payload.size());
  const auto written = (co_await left.write(payload)).value();
  (co_await left.shutdown_write()).value();
  bytes.fill(0);
  const auto consumed = (co_await right.read(bytes)).value();
  const auto eof = (co_await right.read(bytes)).value();
  if (written != payload.size() || consumed != payload.size() || bytes != payload || eof != 0)
    throw std::runtime_error("duplex stream round trip failed");

  auto directory = (co_await faio::fs::read_dir(std::filesystem::temp_directory_path())).value();
  (co_await directory.close()).value();

#if !defined(_WIN32)
  constexpr std::string_view socket_path = "/tmp/faio-contract.sock";
  if (faio::net::unix::UnixAddr::pathname(socket_path).value().path() != socket_path)
    throw std::runtime_error("Unix socket address lost its path");

  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0)
    throw std::runtime_error("pipe creation failed");
  faio::io::unix::OwnedFd writer{pipe_fds[1]};
  faio::io::unix::AsyncFd<> reader{faio::io::unix::OwnedFd{pipe_fds[0]}};
  if (::write(writer.get(), "x", 1) != 1)
    throw std::runtime_error("pipe write failed");
  const auto readiness = (co_await reader.readable()).value();
  char byte{};
  if (!readiness.ready().is_readable() || ::read(reader.native_handle(), &byte, 1) != 1
      || byte != 'x')
    throw std::runtime_error("native descriptor readiness failed");
#endif

  const auto loopback = faio::net::address::parse("127.0.0.1", 0).value();
  const auto addresses = (co_await faio::net::lookup_host("127.0.0.1", 0)).value();
  if (addresses.size() != 1 || addresses.front() != loopback)
    throw std::runtime_error("numeric address resolution failed");
  const auto invalid_host = co_await faio::net::lookup_host(std::string{"x\0y", 3}, 0);
  if (invalid_host || invalid_host.error().native_code() != EINVAL)
    throw std::runtime_error("resolver accepted an embedded NUL");
  auto receiver = faio::net::UdpDatagram::bind(loopback).value();
  auto sender = faio::net::UdpDatagram::bind(loopback).value();
  const auto sent = (co_await sender.send_to(payload, receiver.local_addr().value())).value();
  bytes.fill(0);
  const auto [count, source] = (co_await receiver.recv_from(bytes).set_timeout(3s)).value();
  if (sent != payload.size() || count != payload.size() || bytes != payload
      || source != sender.local_addr().value())
    throw std::runtime_error("UDP datagram round trip failed");
}

}  // namespace

int main() {
  try {
    if (!faio::log::logger())
      throw std::runtime_error("logger unavailable");
    faio::runtime::current_thread_runtime runtime;
    runtime.block_on(stable_contract());
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
