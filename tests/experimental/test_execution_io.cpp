#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <exec/task.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <latch>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace ex = faio::experimental;
using namespace std::chrono_literals;
template <class T, class Error> T take(std::expected<T, Error> result) {
  if (!result) throw std::runtime_error(std::string(result.error().message()));
  return std::move(*result);
}
template <class Error> void take(std::expected<void, Error> result) {
  if (!result) throw std::runtime_error(std::string(result.error().message()));
}
auto socket_pair() {
  std::array<int, 2> descriptors{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data())) throw std::runtime_error("socketpair");
  faio::net::detail::owned_native_socket left{descriptors[0]}, right{descriptors[1]};
  take(faio::net::detail::Socket::prepare(left.get()));
  take(faio::net::detail::Socket::prepare(right.get()));
  faio::net::detail::Socket first{left.get()}, second{right.get()};
  (void)left.release(); (void)right.release();
  return std::pair{std::move(first), std::move(second)};
}
faio::task<void> write_socket(faio::net::detail::Socket& socket) {
  const std::array<char, 4> bytes{'f', 'a', 'i', 'o'};
  if (take(co_await faio::io::send(socket.resource(), bytes.data(), bytes.size(), 0)) != bytes.size())
    throw std::runtime_error("socket send");
}
faio::task<bool> socket_business() {
  auto pair = socket_pair();
  auto writer = faio::spawn(write_socket(pair.second));
  std::array<char, 4> bytes{};
  const auto received = take(co_await faio::io::recv(pair.first.resource(), bytes.data(), bytes.size(), 0).set_timeout(1s));
  co_await writer;
  take(co_await pair.first.close()); take(co_await pair.second.close());
  co_return received == 4 && std::string_view(bytes.data(), bytes.size()) == "faio";
}
faio::task<bool> services_business(std::filesystem::path name) {
  auto file = take(co_await faio::fs::File::open(name, faio::fs::OpenOptions{}.read().write().create_new()));
  const std::string text = "experimental";
  take(co_await file.write_all(std::span<const char>{text}));
  std::array<char, 12> bytes{};
  if (take(co_await file.read_at(bytes, 0)) != text.size()) co_return false;
  take(co_await file.close());
  const auto addresses = take(co_await faio::net::lookup_host(std::string{"localhost"}, 4321));
  const auto worker = std::this_thread::get_id();
  auto computation = faio::spawn_blocking([worker] {
    if (std::this_thread::get_id() == worker) throw std::runtime_error("blocking work ran on IO worker");
    return 42;
  });
  co_return !addresses.empty() && co_await computation == 42
      && std::string_view(bytes.data(), bytes.size()) == text;
}
exec::task<bool> io_workflow(ex::runtime_ref runtime, std::filesystem::path name) {
  if (!co_await runtime.as_sender(socket_business())) co_return false;
  co_return co_await runtime.as_sender(services_business(std::move(name)));
}

// Signal only after the real I/O awaiter has submitted the pending request.
// Cancellation may resume and destroy this object as soon as the latch opens,
// so all state needed after publication is copied before calling count_down.
struct armed_receive {
  faio::io::detail::Recv receive;
  std::latch* armed;
  bool await_ready() const noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> continuation) noexcept {
    auto* signal = armed;
    const bool suspended = receive.await_suspend(continuation);
    signal->count_down();
    return suspended;
  }
  auto await_resume() const noexcept { return receive.await_resume(); }
};
faio::task<int> pending_socket_read(std::latch& armed, std::atomic<unsigned>& cleaned,
                                 std::array<faio::io::detail::resource_ptr, 2>& resources) {
  auto pair = socket_pair();
  resources = {pair.first.resource(), pair.second.resource()};
  std::array<char, 32> payload{};
  struct payload_lifetime {
    std::atomic<unsigned>& count;
    ~payload_lifetime() { count.fetch_add(1, std::memory_order_release); }
  } guard{cleaned};
  const auto result = co_await armed_receive{
      faio::io::recv(pair.first.resource(), payload.data(), payload.size(), 0), &armed};
  if (result || result.error().value() != ECANCELED || result.error().progress() != 0)
    throw std::runtime_error("pending socket cancellation did not drain");
  for (char byte : payload)
    if (byte != 0) throw std::runtime_error("cancelled socket modified borrowed payload");
  // Socket destruction registers cleanup independently of task cancellation.
  // The caller checks real resource retirement after runtime shutdown.
  throw faio::operation_cancelled{};
  co_return 0;
}
exec::task<int> cancelled_io_workflow(ex::runtime_ref runtime, std::latch& armed,
                                    std::atomic<unsigned>& cleaned,
                                    std::array<faio::io::detail::resource_ptr, 2>& resources) {
  co_return co_await runtime.as_sender(pending_socket_read(armed, cleaned, resources));
}
template <class Runtime> bool verify_cancelled_io(Runtime& runtime) {
  std::latch armed{1};
  std::atomic<unsigned> cleaned{};
  std::array<faio::io::detail::resource_ptr, 2> resources;
  std::jthread stopper([&] { armed.wait(); runtime.request_stop(); });
  const auto result = runtime.run(cancelled_io_workflow(runtime.ref(), armed, cleaned, resources));
  stopper.join();
  const bool drained = !result && cleaned.load(std::memory_order_acquire) == 1
      && runtime.ref().context().external_host().quiescent();
  runtime.shutdown();
  return drained && resources[0]->fd() == -1 && resources[1]->fd() == -1
      && resources[0]->owner->quiescent() && resources[1]->owner->quiescent();
}
template <class Runtime> bool verify(Runtime& runtime, const char* suffix) {
  auto name = std::filesystem::temp_directory_path() / ("faio-experimental-" + std::to_string(::getpid()) + suffix);
  struct cleanup { std::filesystem::path name; ~cleanup() { std::error_code error; std::filesystem::remove(name, error); } } guard{name};
  auto result = runtime.run(io_workflow(runtime.ref(), name));
  return result && std::get<0>(*result);
}
int main() {
  auto single = ex::current_thread_defaults();
  auto many = ex::multi_thread_defaults(); many.workers = 4;
  const auto* backend = std::getenv("FAIO_TEST_IO_BACKEND");
  const auto chosen = backend && std::string_view(backend) == "uring" ? faio::runtime::io_backend::IO_URING : faio::runtime::io_backend::IO_EPOLL;
  single.io_backend = chosen; many.io_backend = chosen;
  ex::current_thread_runtime current{single}; ex::multi_thread_runtime multi{many};
  many.workers = 1;
  ex::multi_thread_runtime one{many};
  return verify(current, "-current") && verify(multi, "-multi")
      && verify_cancelled_io(current) && verify_cancelled_io(one)
      && verify_cancelled_io(multi) ? 0 : 1;
}
