/**
 * @file tcp_echo_load.cpp
 * @brief 独立 POSIX TCP 闭环负载发生器；同一个可执行文件用于测量 faio 与 Tokio。
 *
 * 每条连接只有一个在途请求，收到完整且逐字节一致的回包才发下一包。
 * 计时覆盖发送开始至回包完整到达；不包含连接建立。预热样本不进入统计。
 * 输出真实请求延迟的 p50/p95/p99/p99.9，保留客户端与服务端分离的线程数。
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;
constexpr std::size_t sample_limit_per_thread = 4'000'000;

/** @brief 每连接的部分发送/接收状态；描述符在退出时关闭。 */
struct connection {
    int fd{-1};
    std::size_t sent{0}, received{0};
    bool active{false}; ///< 首次 send 尝试开始计时；EAGAIN 重试不得重置时钟。
    clock_type::time_point started{};
    std::vector<char> input;
    explicit connection(std::size_t payload) : input(payload) {}
    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    connection(connection&& other) noexcept : fd(std::exchange(other.fd, -1)),
        sent(other.sent), received(other.received), active(other.active), started(other.started), input(std::move(other.input)) {}
    ~connection() { if (fd >= 0) ::close(fd); }
};

/** @brief 每个负载线程只写自己的统计，join 后主线程合并。 */
struct thread_result {
    std::vector<std::int64_t> latency_ns;
    std::size_t errors{0};
    std::string failure;
};

/** @brief 建立连接后设置非阻塞；连接时间不计入请求延迟。 */
int connect_to(const sockaddr* address, socklen_t length) {
    const int fd = ::socket(address->sa_family, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error(std::strerror(errno));
    if (::connect(fd, address, length) != 0) {
        const auto error = std::string(std::strerror(errno));
        ::close(fd);
        throw std::runtime_error("connect: " + error);
    }
    const int enabled = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0 ||
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
        ::close(fd);
        throw std::runtime_error("socket configuration failed");
    }
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    return fd;
}

/** @brief 驱动部分 IO；poll 的就绪是假就绪时保留当前偏移。 */
template<class Completion>
void measure(const sockaddr_storage& address, socklen_t length, std::size_t count,
             const std::vector<char>& payload, double duration, double warmup,
             std::barrier<Completion>& start_gate, clock_type::time_point& epoch, thread_result& result) {
    bool joined_gate = false;
    try {
        std::vector<connection> sockets;
        std::vector<pollfd> events;
        sockets.reserve(count); events.reserve(count);
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
        while (clock_type::now() < end) {
            const int ready = ::poll(events.data(), events.size(), 20);
            if (ready < 0) { if (errno == EINTR) continue; throw std::runtime_error("poll failed"); }
            for (std::size_t i = 0; i < sockets.size(); ++i) {
                if (!events[i].revents) continue;
                auto& item = sockets[i];
                if (events[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                    throw std::runtime_error("connection closed during measurement");
                if ((events[i].revents & POLLOUT) && item.sent < payload.size()) {
                    if (!item.active) { item.started = clock_type::now(); item.active = true; }
#ifdef MSG_NOSIGNAL
                    constexpr int flags = MSG_NOSIGNAL;
#else
                    constexpr int flags = 0;
#endif
                    const auto n = ::send(item.fd, payload.data() + item.sent, payload.size() - item.sent, flags);
                    if (n > 0) item.sent += static_cast<std::size_t>(n);
                    else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
                        throw std::runtime_error("send failed");
                    if (item.sent == payload.size()) events[i].events = POLLIN;
                }
                if ((events[i].revents & POLLIN) && item.received < payload.size()) {
                    const auto n = ::recv(item.fd, item.input.data() + item.received, payload.size() - item.received, 0);
                    if (n > 0) item.received += static_cast<std::size_t>(n);
                    else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
                        throw std::runtime_error("recv failed");
                    if (item.received == payload.size()) {
                        const auto now = clock_type::now();
                        if (std::memcmp(item.input.data(), payload.data(), payload.size()) != 0)
                            throw std::runtime_error("echo payload mismatch");
                        if (item.started >= measured_begin && now <= end) {
                            if (result.latency_ns.size() == sample_limit_per_thread)
                                throw std::runtime_error("sample limit reached; reduce measurement duration");
                            result.latency_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(now - item.started).count());
                        }
                        item.sent = item.received = 0;
                        item.active = false;
                        events[i].events = POLLOUT;
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        result.failure = error.what();
        ++result.errors;
        // 建连失败也退出 barrier，避免其他负载线程永久等候。
        if (!joined_gate) start_gate.arrive_and_drop();
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 8) throw std::invalid_argument("usage: tcp_echo_load host port client_threads connections payload_bytes seconds warmup_seconds");
        const auto threads = std::stoul(argv[3]);
        const auto connections = std::stoul(argv[4]);
        const auto bytes = std::stoul(argv[5]);
        const auto seconds = std::stod(argv[6]);
        const auto warmup = std::stod(argv[7]);
        if (!threads || threads > 256 || connections < threads || connections > 100'000 ||
            !bytes || bytes > 1'048'576 || seconds <= 0 || seconds > 600 || warmup < 0)
            throw std::invalid_argument("invalid load configuration");
        addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (::getaddrinfo(argv[1], argv[2], &hints, &found) != 0 || !found)
            throw std::runtime_error("address resolution failed");
        sockaddr_storage address{};
        const auto address_length = static_cast<socklen_t>(found->ai_addrlen);
        std::memcpy(&address, found->ai_addr, address_length);
        ::freeaddrinfo(found);
        std::vector<char> payload(bytes);
        for (std::size_t i = 0; i < bytes; ++i) payload[i] = static_cast<char>(i % 251);
        clock_type::time_point epoch;
        std::barrier start_gate(static_cast<std::ptrdiff_t>(threads + 1), [&]() noexcept { epoch = clock_type::now(); });
        std::vector<thread_result> results(threads);
        std::vector<std::jthread> clients;
        for (std::size_t i = 0; i < threads; ++i) {
            const auto count = connections / threads + (i < connections % threads ? 1 : 0);
            clients.emplace_back([&, i, count] {
                measure(address, address_length, count, payload, seconds, warmup, start_gate, epoch, results[i]);
            });
        }
        start_gate.arrive_and_wait();
        clients.clear();
        std::vector<std::int64_t> samples;
        std::size_t errors = 0;
        for (auto& result : results) {
            errors += result.errors;
            if (!result.failure.empty()) std::cerr << result.failure << '\n';
            samples.insert(samples.end(), result.latency_ns.begin(), result.latency_ns.end());
        }
        if (errors || samples.empty()) throw std::runtime_error("incomplete measurement");
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&](double p) { return samples[static_cast<std::size_t>(p * (samples.size() - 1))]; };
        std::cout << "requests,requests_per_sec,payload_bytes,connections,client_threads,p50_ns,p95_ns,p99_ns,p999_ns,max_ns,errors\n";
        std::cout << samples.size() << ',' << samples.size() / seconds << ',' << bytes << ',' << connections << ',' << threads
                  << ',' << percentile(.50) << ',' << percentile(.95) << ',' << percentile(.99) << ',' << percentile(.999)
                  << ',' << samples.back() << ',' << errors << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
