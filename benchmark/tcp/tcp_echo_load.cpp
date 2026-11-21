/**
 * @file tcp_echo_load.cpp
 * @brief 独立 TCP 闭环负载发生器；同一个可执行文件用于测量 faio 与 Tokio。
 */
#include <cstddef>
#include <algorithm>
#include <barrier>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sched.h>
#endif

namespace {
using clock_type = std::chrono::steady_clock;
constexpr std::size_t sample_limit_per_thread = 4'000'000;

#ifdef _WIN32
using socket_handle = SOCKET;
using socket_length = int;
using poll_descriptor = WSAPOLLFD;
constexpr socket_handle invalid_socket = INVALID_SOCKET;

/**  @brief 所有客户端线程结束后才清理 Winsock，避免清理时使线程仍在使用的套接字失效。 */
struct socket_system {
  /** @brief 初始化 Windows 套接字子系统。 */
  socket_system() {
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data))
      throw std::runtime_error("WSAStartup failed");
  }

  /** @brief 释放 Windows 套接字子系统使用计数。 */
  ~socket_system() { ::WSACleanup(); }
};

/** @brief 关闭一个平台原生套接字描述符。 */
void close_socket(socket_handle fd) noexcept {
  ::closesocket(fd);
}

/** @brief 检查当前套接字错误是否允许重试。 */
bool retry_socket_error() noexcept {
  const auto code = ::WSAGetLastError();
  return code == WSAEINTR || code == WSAEWOULDBLOCK;
}

/** @brief 判断套接字调用是否被中断。 */
bool interrupted_socket_call() noexcept { return ::WSAGetLastError() == WSAEINTR; }

/** @brief 等待负载连接的读写就绪事件。 */
int poll_sockets(poll_descriptor* events, std::size_t count, int timeout) {
  return ::WSAPoll(events, static_cast<ULONG>(count), timeout);
}
#else
using socket_handle = int;
using socket_length = socklen_t;
using poll_descriptor = pollfd;
constexpr socket_handle invalid_socket = -1;

/** @brief 管理平台套接字子系统的进程级初始化。 */
struct socket_system {};

/** @brief 关闭一个平台原生套接字描述符。 */
void close_socket(socket_handle fd) noexcept {
  ::close(fd);
}

/** @brief 检查当前套接字错误是否允许重试。 */
bool retry_socket_error() noexcept {
  return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

/** @brief 判断套接字调用是否被中断。 */
bool interrupted_socket_call() noexcept { return errno == EINTR; }

/** @brief 等待负载连接的读写就绪事件。 */
int poll_sockets(poll_descriptor* events, std::size_t count, int timeout) {
  return ::poll(events, count, timeout);
}
#endif

/**  @brief 各连接在部分发送或接收后的状态；描述符在退出时关闭。 */
struct connection {
  socket_handle fd{invalid_socket};
  std::size_t sent{0}, received{0};
  bool active{false};  /**< 首次 send 尝试开始计时；EAGAIN 重试不得重置时钟。 */
  clock_type::time_point started{};
  std::vector<char> input;

  /** @brief 为连接的响应负载预分配接收缓冲。 */
  explicit connection(std::size_t payload) : input(payload) {}

  /** @brief 禁止复制套接字描述符的所有权。 */
  connection(const connection&) = delete;

  /** @brief 禁止复制套接字描述符的所有权。 */
  connection& operator=(const connection&) = delete;

  /** @brief 转交连接描述符与未完成传输状态。 */
  connection(connection&& other) noexcept
      : fd(std::exchange(other.fd, invalid_socket)),
        sent(other.sent),
        received(other.received),
        active(other.active),
        started(other.started),
        input(std::move(other.input)) {}

  /** @brief 关闭当前连接拥有的套接字描述符。 */
  ~connection() {
    if (fd != invalid_socket)
      close_socket(fd);
  }
};

/**  @brief 每个负载线程只写自己的统计，join 后主线程合并。 */
struct thread_result {
  std::vector<std::int64_t> latency_ns;
  std::size_t errors{0};
  std::size_t completed_in_window{0};
  std::string failure;
};

/** @brief 建立 TCP 连接并配置无延迟与非阻塞模式。 */
socket_handle connect_to(const sockaddr* address, socket_length length) {
  const auto fd = ::socket(address->sa_family, SOCK_STREAM, 0);
  if (fd == invalid_socket)
    throw std::runtime_error("socket creation failed");
  if (::connect(fd, address, length) != 0) {
#ifdef _WIN32
    const auto error = std::to_string(::WSAGetLastError());
#else
    const auto error = std::string(std::strerror(errno));
#endif
    close_socket(fd);
    throw std::runtime_error("connect: " + error);
  }
  const int enabled = 1;
#ifdef _WIN32
  u_long nonblocking = 1;
  if (::setsockopt(
          fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled))
          != 0
      || ::ioctlsocket(fd, FIONBIO, &nonblocking) != 0) {
#else
  if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0
      || ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
#endif
    close_socket(fd);
    throw std::runtime_error("socket configuration failed");
  }
#ifdef SO_NOSIGPIPE
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
  return fd;
}

/** @brief 发送请求的剩余字节，返回下一次需要等待的事件。 */
template <bool Immediate>
short send_request(connection& item, const std::vector<char>& payload) {
#ifdef MSG_NOSIGNAL
  constexpr int flags = MSG_NOSIGNAL;
#else
  constexpr int flags = 0;
#endif
  do {
    const auto n = ::send(item.fd,
                          payload.data() + item.sent,
                          static_cast<int>(payload.size() - item.sent),
                          flags);
    if (n > 0) {
      item.sent += static_cast<std::size_t>(n);
      if (item.sent == payload.size())
        return POLLIN;
      continue;
    }
    if (n == 0 || !retry_socket_error())
      throw std::runtime_error("send failed");
    if (!Immediate || !interrupted_socket_call())
      break;
  } while (Immediate);
  return POLLOUT;
}

/** @brief 驱动持久连接并记录请求时延，部分发送或接收后保留当前偏移。 */
template <bool Immediate, class Completion>
void measure(const sockaddr_storage& address,
             socket_length length,
             std::size_t count,
             const std::vector<char>& payload,
             double duration,
             double warmup,
             [[maybe_unused]] int cpu,
             std::barrier<Completion>& start_gate,
             clock_type::time_point& epoch,
             thread_result& result) {
  bool joined_gate = false;
  try {
#ifdef __linux__
    if (cpu >= 0) {
      cpu_set_t affinity;
      CPU_ZERO(&affinity);
      CPU_SET(cpu, &affinity);
      if (::sched_setaffinity(0, sizeof(affinity), &affinity) < 0)
        throw std::runtime_error(std::string{"sched_setaffinity: "} + std::strerror(errno));
    }
#endif
    std::vector<connection> sockets;
    std::vector<poll_descriptor> events;
    sockets.reserve(count);
    events.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      sockets.emplace_back(payload.size());
      sockets.back().fd = connect_to(reinterpret_cast<const sockaddr*>(&address), length);
      events.push_back({sockets.back().fd, POLLOUT, 0});
    }
    result.latency_ns.reserve(250'000);
    joined_gate = true;
    start_gate.arrive_and_wait();
    const auto measured_begin = epoch + std::chrono::duration<double>(warmup);
    const auto end = measured_begin + std::chrono::duration<double>(duration);
    std::size_t outstanding{};
    const auto drain_deadline = end + std::chrono::seconds{3};
    const auto send = [&](std::size_t i) {
      auto& item = sockets[i];
      if (!item.active) {
        item.started = clock_type::now();
        if (item.started >= end) {
          events[i].events = 0;
          return false;
        }
        item.active = true;
        ++outstanding;
      }
      events[i].events = send_request<Immediate>(item, payload);
      return true;
    };
    if constexpr (Immediate) {
      for (std::size_t i = 0; i < sockets.size(); ++i)
        send(i);
    }
    while (true) {
      const auto before_poll = clock_type::now();
      if (before_poll >= end && outstanding == 0)
        break;
      if (before_poll >= drain_deadline)
        throw std::runtime_error("unfinished request after measurement drain timeout");
      const int ready = poll_sockets(events.data(), events.size(), 20);
      if (ready < 0) {
        if (retry_socket_error())
          continue;
        throw std::runtime_error("poll failed");
      }
      for (std::size_t i = 0; i < sockets.size(); ++i) {
        if (!events[i].revents)
          continue;
        auto& item = sockets[i];
        if (events[i].revents & (POLLERR | POLLHUP | POLLNVAL))
          throw std::runtime_error("connection closed during measurement");
        if ((events[i].revents & POLLOUT) && item.sent < payload.size() && !send(i))
          continue;
        if ((events[i].revents & POLLIN) && item.received < payload.size()) {
          const auto n = ::recv(item.fd,
                                item.input.data() + item.received,
                                static_cast<int>(payload.size() - item.received),
                                0);
          if (n > 0)
            item.received += static_cast<std::size_t>(n);
          else if (n == 0 || !retry_socket_error())
            throw std::runtime_error("recv failed");
          if (item.received == payload.size()) {
            const auto now = clock_type::now();
            if (std::memcmp(item.input.data(), payload.data(), payload.size()) != 0)
              throw std::runtime_error("echo payload mismatch");
            if (item.started >= measured_begin && item.started < end) {
              if (now <= end)
                ++result.completed_in_window;
              if (result.latency_ns.size() == sample_limit_per_thread)
                throw std::runtime_error("sample limit reached; reduce measurement duration");
              result.latency_ns.push_back(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(now - item.started).count());
            }
            item.sent = item.received = 0;
            item.active = false;
            --outstanding;
            if constexpr (Immediate)
              send(i);
            else
              events[i].events = now < end ? POLLOUT : 0;
          }
        }
      }
    }
  } catch (const std::exception& error) {
    result.failure = error.what();
    ++result.errors;
    if (!joined_gate)
      start_gate.arrive_and_drop();
  }
}
}

/** @brief 生成持久 TCP 连接负载并汇总 QPS 及延迟分位数。 */
int main(int argc, char** argv) {
  try {
    [[maybe_unused]] socket_system system;
    if (argc < 8)
      throw std::invalid_argument(
          "usage: tcp_echo_load host port client_threads connections payload_bytes seconds warmup_seconds "
          "[--immediate] [--pin-clients]");
    bool immediate = false;
    bool pin_clients = false;
    for (int i = 8; i < argc; ++i) {
      const std::string_view option{argv[i]};
      if (option == "--immediate" && !immediate)
        immediate = true;
      else if (option == "--pin-clients" && !pin_clients)
        pin_clients = true;
      else
        throw std::invalid_argument("unknown or duplicate option: " + std::string{option});
    }
    const auto threads = std::stoul(argv[3]);
    const auto connections = std::stoul(argv[4]);
    const auto bytes = std::stoul(argv[5]);
    const auto seconds = std::stod(argv[6]);
    const auto warmup = std::stod(argv[7]);
    if (!threads || threads > 256 || connections < threads || connections > 100'000 || !bytes
        || bytes > 1'048'576 || seconds <= 0 || seconds > 600 || warmup < 0)
      throw std::invalid_argument("invalid load configuration");
    std::vector<int> client_cpus;
    if (pin_clients) {
#ifdef __linux__
      cpu_set_t allowed;
      CPU_ZERO(&allowed);
      if (::sched_getaffinity(0, sizeof(allowed), &allowed) < 0)
        throw std::runtime_error(std::string{"sched_getaffinity: "} + std::strerror(errno));
      for (int cpu = 0; cpu < CPU_SETSIZE && client_cpus.size() < threads; ++cpu) {
        if (CPU_ISSET(cpu, &allowed))
          client_cpus.push_back(cpu);
      }
      if (client_cpus.size() < threads)
        throw std::invalid_argument("--pin-clients requires one allowed CPU per client thread");
#else
      throw std::invalid_argument("--pin-clients requires Linux");
#endif
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (::getaddrinfo(argv[1], argv[2], &hints, &found) != 0 || !found)
      throw std::runtime_error("address resolution failed");
    sockaddr_storage address{};
    const auto address_length = static_cast<socket_length>(found->ai_addrlen);
    std::memcpy(&address, found->ai_addr, address_length);
    ::freeaddrinfo(found);
    std::vector<char> payload(bytes);
    for (std::size_t i = 0; i < bytes; ++i)
      payload[i] = static_cast<char>(i % 251);
    clock_type::time_point epoch;
    auto set_epoch = [&]() noexcept { epoch = clock_type::now(); };
    std::barrier start_gate(static_cast<std::ptrdiff_t>(threads + 1), set_epoch);
    std::vector<thread_result> results(threads);
    std::vector<std::jthread> clients;
    const auto run_measure =
        immediate ? &measure<true, decltype(set_epoch)> : &measure<false, decltype(set_epoch)>;
    for (std::size_t i = 0; i < threads; ++i) {
      const auto count = connections / threads + (i < connections % threads ? 1 : 0);
      const auto cpu = client_cpus.empty() ? -1 : client_cpus[i];
      clients.emplace_back([&, i, count, cpu] {
        run_measure(address,
                    address_length,
                    count,
                    payload,
                    seconds,
                    warmup,
                    cpu,
                    start_gate,
                    epoch,
                    results[i]);
      });
    }
    start_gate.arrive_and_wait();
    clients.clear();
    std::vector<std::int64_t> samples;
    std::size_t errors = 0, completed_in_window = 0;
    for (auto& result : results) {
      errors += result.errors;
      completed_in_window += result.completed_in_window;
      if (!result.failure.empty())
        std::cerr << result.failure << '\n';
      samples.insert(samples.end(), result.latency_ns.begin(), result.latency_ns.end());
    }
    if (errors || samples.empty())
      throw std::runtime_error("incomplete measurement");
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double p) {
      return samples[static_cast<std::size_t>(p * (samples.size() - 1))];
    };
    std::cout
        << "requests,requests_per_sec,payload_bytes,connections,client_threads,p50_ns,p95_ns,p99_ns,p999_ns,max_ns,errors,avg_ns,latency_samples\n";
    std::cout << completed_in_window << ',' << completed_in_window / seconds << ',' << bytes << ','
              << connections << ',' << threads << ',' << percentile(.50) << ',' << percentile(.95)
              << ',' << percentile(.99) << ',' << percentile(.999) << ',' << samples.back() << ','
              << errors << ','
              << std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size() << ','
              << samples.size() << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
