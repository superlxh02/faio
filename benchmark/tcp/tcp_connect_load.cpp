/**
 * @file benchmark/tcp/tcp_connect_load.cpp
 * @brief TCP 短连接负载生成器。
 */
#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <netinet/tcp.h>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using clock = std::chrono::steady_clock;

/** @brief 短连接负载的独占套接字描述符。 */
class socket {
 public:
  /** @brief 接管短连接套接字描述符。 */
  explicit socket(int descriptor) noexcept : descriptor_{descriptor} {}

  /** @brief 禁止复制套接字描述符的所有权。 */
  socket(const socket&) = delete;
  /** @brief 禁止复制套接字描述符的所有权。 */
  socket& operator=(const socket&) = delete;

  /** @brief 关闭短连接负载拥有的描述符。 */
  ~socket() {
    if (descriptor_ >= 0)
      (void)::close(descriptor_);
  }

  /** @brief 借用当前套接字的原生描述符。 */
  int get() const noexcept { return descriptor_; }

 private:
  int descriptor_;
};

/** @brief 负载线程独占的连接延迟、请求延迟与错误统计。 */
struct result_type {
  std::size_t errors{};
  std::size_t timeouts{};
  std::size_t completed_in_window{};
  std::vector<std::int64_t> connect_ns;
  std::vector<std::int64_t> request_ns;
};

/** @brief 完整发送并接收一次负载，校验回显字节。 */
bool transfer(int descriptor, std::span<const char> sent, std::span<char> received) {
  std::size_t done{};
  while (done < sent.size()) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto count = ::send(descriptor, sent.data() + done, sent.size() - done, flags);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    done += static_cast<std::size_t>(count);
  }
  done = 0;
  while (done < received.size()) {
    const auto count = ::recv(descriptor, received.data() + done, received.size() - done, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    done += static_cast<std::size_t>(count);
  }
  return std::equal(sent.begin(), sent.end(), received.begin());
}

/** @brief 生成短连接请求并记录连接建立与请求延迟。 */
template <class Gate>
void run_client(const sockaddr_storage& address,
                socklen_t address_length,
                const std::vector<char>& payload,
                double seconds,
                double warmup,
                Gate& gate,
                clock::time_point& epoch,
                result_type& result) {
  std::vector<char> received(payload.size());
  result.connect_ns.reserve(100'000);
  result.request_ns.reserve(100'000);
  gate.arrive_and_wait();
  const auto begin = epoch + std::chrono::duration<double>(warmup);
  const auto end = begin + std::chrono::duration<double>(seconds);
  while (clock::now() < end) {
    const auto start = clock::now();
    socket socket{::socket(address.ss_family, SOCK_STREAM, 0)};
    if (socket.get() < 0) {
      if (start >= begin)
        ++result.errors;
      continue;
    }
    const int enabled = 1;
    const timeval timeout{2, 0};
    if (::setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0
        || ::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0
        || ::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0
        || ::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), address_length)
               < 0) {
      if (start >= begin) {
        ++result.errors;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
          ++result.timeouts;
      }
      continue;
    }
    const auto connected = clock::now();
    const auto complete = transfer(socket.get(), payload, received);
    const auto finished = clock::now();
    if (start >= begin && start < end) {
      if (!complete) {
        ++result.errors;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
          ++result.timeouts;
      } else {
        if (finished <= end)
          ++result.completed_in_window;
        result.connect_ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(connected - start).count());
        result.request_ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - start).count());
      }
    }
  }
}
}  // namespace

/** @brief 生成 TCP 短连接负载并汇总连接与请求延迟。 */
int main(int argc, char** argv) {
  try {
    if (argc != 7)
      throw std::invalid_argument(
          "usage: tcp_connect_load host port concurrent_clients payload_bytes seconds warmup_seconds");
    const auto clients = std::stoul(argv[3]);
    const auto bytes = std::stoul(argv[4]);
    const auto seconds = std::stod(argv[5]);
    const auto warmup = std::stod(argv[6]);
    if (!clients || clients > 256 || !bytes || bytes > 1'048'576 || seconds <= 0 || seconds > 60
        || warmup < 0 || warmup > 60)
      throw std::invalid_argument("invalid load configuration");
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved{};
    if (::getaddrinfo(argv[1], argv[2], &hints, &resolved) != 0)
      throw std::runtime_error("address resolution failed");
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> resolution{resolved, ::freeaddrinfo};
    sockaddr_storage address{};
    const auto address_length = static_cast<socklen_t>(resolved->ai_addrlen);
    std::memcpy(&address, resolved->ai_addr, address_length);
    std::vector<char> payload(bytes);
    for (std::size_t i = 0; i < payload.size(); ++i)
      payload[i] = static_cast<char>(i % 251);
    clock::time_point epoch;
    std::barrier gate{static_cast<std::ptrdiff_t>(clients + 1),
                      [&]() noexcept { epoch = clock::now(); }};
    std::vector<result_type> results(clients);
    std::vector<std::jthread> workers;
    workers.reserve(clients);
    for (std::size_t i = 0; i < clients; ++i)
      workers.emplace_back([&, i] {
        run_client(address, address_length, payload, seconds, warmup, gate, epoch, results[i]);
      });
    gate.arrive_and_wait();
    workers.clear();
    std::size_t errors{}, timeouts{}, completed_in_window{};
    std::vector<std::int64_t> connect_samples, request_samples;
    for (const auto& result : results) {
      errors += result.errors;
      timeouts += result.timeouts;
      completed_in_window += result.completed_in_window;
      connect_samples.insert(
          connect_samples.end(), result.connect_ns.begin(), result.connect_ns.end());
      request_samples.insert(
          request_samples.end(), result.request_ns.begin(), result.request_ns.end());
    }
    if (connect_samples.empty())
      throw std::runtime_error("no complete verified connections");
    std::sort(connect_samples.begin(), connect_samples.end());
    std::sort(request_samples.begin(), request_samples.end());
    const auto percentile = [](const auto& samples, double p) {
      return samples[static_cast<std::size_t>(p * (samples.size() - 1))];
    };
    const auto mean = [](const auto& samples) {
      return std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
    };
    std::cout
        << "connections,connections_per_sec,concurrent_clients,payload_bytes,connect_p50_ns,connect_p99_ns,request_p50_ns,request_p99_ns,errors,timeouts,connect_avg_ns,request_avg_ns,completed_in_window\n"
        << connect_samples.size() << ',' << completed_in_window / seconds << ',' << clients << ','
        << bytes << ',' << percentile(connect_samples, .50) << ','
        << percentile(connect_samples, .99) << ',' << percentile(request_samples, .50) << ','
        << percentile(request_samples, .99) << ',' << errors << ',' << timeouts << ','
        << mean(connect_samples) << ',' << mean(request_samples) << ',' << completed_in_window
        << '\n';
    return errors ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
