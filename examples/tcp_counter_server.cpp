/**
 * 复杂 TCP 场景：多个客户端共享的内存计数器/键值服务。
 * 可以用于访问量、订单数等整数统计，支持 SET / GET / INCR / DEL / QUIT。
 * 请求和响应以换行分帧，一条连接可以连续发送多条命令。
 * 默认监听 127.0.0.1:8081；--self-test 在临时端口运行完整客户端场景后退出。
 * 这里只用数据结构和普通函数组织代码，没有通用服务框架或协议类。
 */
#include "faio/faio.hpp"
#include "faio/log.hpp"
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace {
constexpr std::size_t max_request_bytes =
    1024; // 包括结尾换行，约束单条请求的内存。
constexpr std::size_t max_keys = 256;

struct counter_store {
  faio::sync::mutex mutex;
  std::map<std::string, std::int64_t> values;
};

faio::task<void> deadline(std::chrono::seconds duration) {
  co_await faio::time::sleep(duration);
}

// 解析并执行一条命令，返回完整响应。网络读写留在连接协程中，避免持锁等待 I/O。
faio::task<std::string> execute_command(counter_store &store,
                                        std::string request) {
  std::istringstream input{request};
  std::string command, key, argument, extra;
  input >> command;
  if (command == "QUIT") {
    if (input >> extra)
      co_return "ERR arguments\n";
    co_return "BYE\n";
  }
  if (command != "SET" && command != "GET" && command != "INCR" &&
      command != "DEL")
    co_return "ERR command\n";
  if (!(input >> key) || key.size() > 32)
    co_return "ERR key\n";

  std::int64_t value = 0;
  if (command == "SET") {
    if (!(input >> argument))
      co_return "ERR arguments\n";
    // from_chars 必须消费整个字符串，拒绝 12abc 和超出 int64 范围的输入。
    const auto [end, error] = std::from_chars(
        argument.data(), argument.data() + argument.size(), value);
    if (error != std::errc{} || end != argument.data() + argument.size())
      co_return "ERR integer\n";
  }
  if (input >> extra)
    co_return "ERR arguments\n";

  // 多个连接协程可能在不同 worker 上执行。整个 INCR 的读、加、写在同一把
  // 协程 mutex 内完成，不能把 GET 和 SET 分别加锁后拼成一个递增操作。
  auto guard = co_await store.mutex.scoped_lock();
  auto found = store.values.find(key);
  if (command == "GET")
    co_return found == store.values.end()
        ? "NOT_FOUND\n"
        : "VALUE " + std::to_string(found->second) + "\n";
  if (command == "DEL")
    co_return store.values.erase(key) ? "DELETED\n" : "NOT_FOUND\n";
  if (found == store.values.end()) {
    if (store.values.size() == max_keys)
      co_return "ERR capacity\n";
    found = store.values.emplace(key, 0).first;
  }
  if (command == "SET") {
    found->second = value;
    co_return "OK\n";
  }
  if (found->second == std::numeric_limits<std::int64_t>::max())
    co_return "ERR overflow\n";
  ++found->second; // 不存在的键从 0 开始递增。
  co_return "VALUE " + std::to_string(found->second) + "\n";
  // guard 析构解锁；响应交给调用者后才进行网络发送。
}

// 一个连接对应一个协程；请求按连接内顺序处理，多个连接之间并发执行。
faio::task<void> counter_session(faio::net::TcpStream stream,
                                 counter_store &store,
                                 std::chrono::seconds request_timeout) {
  try {
    // BufReader 保留多读出来的后续命令。不能绕过 reader 再直接 stream.read，
    // 否则会丢失它缓存的字节。read_line
    // 处理拆包和多条命令合并到一次读取的情况。
    faio::io::BufReader reader{stream, 512};
    for (;;) {
      std::string line;
      // 给整条请求设置期限，而非每收到一个字节就重新计时；慢速发送也有上限。
      const auto ready = co_await faio::select(
          faio::io::read_line(reader, line, max_request_bytes),
          deadline(request_timeout));
      std::string response;
      bool disconnect = false;
      if (ready.index == 1) {
        response = "ERR timeout\n";
        disconnect = true;
      } else {
        const auto &read = std::get<0>(ready.value);
        if (!read) {
          if (read.error().value() != EMSGSIZE) {
            faio::log::logger()->warn("计数器连接读取失败：{}",
                                      read.error().message());
            co_return;
          }
          response = "ERR too_long\n";
          disconnect = true; // 不再解析超长请求的残余字节，防止协议错位。
        } else if (*read == 0) {
          co_return; // 没有未完成命令的正常 EOF。
        } else if (line.back() != '\n') {
          response = "ERR incomplete\n";
          disconnect = true; // 客户端半关闭时留下的不完整命令不能执行。
        } else {
          line.pop_back();
          if (!line.empty() && line.back() == '\r')
            line.pop_back(); // 同时支持 LF 和 CRLF。
          response = co_await execute_command(store, std::move(line));
          disconnect = response == "BYE\n";
        }
      }
      // write_all 处理短写；select 为整个发送设置 5 秒期限，慢客户端不会无限
      // 占用此连接协程。select 返回前排空落选分支，response 的借用仍然有效。
      const auto sent =
          co_await faio::select(stream.write_all(response), deadline(5s));
      if (sent.index == 1) {
        faio::log::logger()->warn("计数器响应发送超时，关闭连接");
        co_return;
      }
      const auto &written = std::get<0>(sent.value);
      if (!written) {
        faio::log::logger()->warn("计数器响应发送失败：{}",
                                  written.error().message());
        co_return;
      }
      if (disconnect)
        co_return; // 发送 BYE 或协议终止错误后，stream 析构关闭连接。
    }
  } catch (const std::exception &error) {
    // 正常服务使用 detached 连接任务；所有异常在连接内处理，不影响其他客户端。
    faio::log::logger()->warn("计数器连接退出：{}", error.what());
  }
}

faio::task<void> accept_clients(faio::net::TcpListener &listener,
                                counter_store &store,
                                std::size_t client_limit = 0) {
  std::vector<faio::join_handle<void>> sessions;
  for (std::size_t count = 0; client_limit == 0 || count < client_limit;
       ++count) {
    auto accept = listener.accept();
    if (client_limit != 0)
      accept.set_timeout(
          5s); // 有限演示出错时也能退出，不会永久等待缺席客户端。
    auto accepted = co_await accept;
    if (!accepted)
      throw std::runtime_error("计数器 accept 失败：" +
                               std::string{accepted.error().message()});
    auto &[stream, peer] = *accepted;
    faio::log::logger()->info("计数器客户端接入：{}", peer.to_string());
    if (client_limit == 0)
      faio::spawn_detached(counter_session(std::move(stream), store, 30s));
    else
      sessions.push_back(
          faio::spawn(counter_session(std::move(stream), store, 3s)));
  }
  // 有限演示要明确等全部连接退出；长期服务直接 detached，不积攒历史句柄。
  for (auto &session : sessions)
    co_await session;
}

faio::task<void> example_tcp_counter_server(counter_store &store) {
  const faio::net::address address{faio::net::v4addr{127, 0, 0, 1}, 8081};
  auto bound = faio::net::TcpListener::bind(address);
  if (!bound)
    throw std::runtime_error("计数器绑定失败：" +
                             std::string{bound.error().message()});
  auto listener = std::move(*bound);
  faio::log::logger()->info("TCP 计数器服务监听 {}，用 nc 连接；Ctrl+C 结束",
                            address.to_string());
  co_await accept_clients(listener, store);
}

// 以下客户端只用于
// --self-test，服务端使用示例从上面的三个普通协程函数阅读即可。
faio::task<void> increment_client(faio::net::address address) {
  auto connected = co_await faio::net::TcpStream::connect(address);
  if (!connected)
    throw std::runtime_error("演示客户端连接失败");
  auto stream = std::move(*connected);
  faio::io::BufReader reader{stream};
  // 连续写五条命令再读取五条响应，展示同一连接中的流水线请求。
  const std::string requests =
      "INCR visits\nINCR visits\nINCR visits\nINCR visits\nINCR visits\nQUIT\n";
  if (!(co_await stream.write_all(requests)))
    throw std::runtime_error("演示客户端发送失败");
  for (int i = 0; i < 6; ++i) {
    std::string response;
    auto read = co_await faio::io::read_line(reader, response, 128);
    if (!read || *read == 0 ||
        (i < 5 ? !response.starts_with("VALUE ") : response != "BYE\n"))
      throw std::runtime_error("递增客户端收到错误响应");
  }
}

faio::task<void> verify_client(faio::net::address address) {
  auto connected = co_await faio::net::TcpStream::connect(address);
  if (!connected)
    throw std::runtime_error("校验客户端连接失败");
  auto stream = std::move(*connected);
  // 刻意分两次发送第一条命令；服务不能把首次 read 当成完整请求。
  if (!(co_await stream.write_all(std::string_view{"SET stock "})))
    throw std::runtime_error("拆分请求发送失败");
  co_await faio::time::sleep(20ms);
  if (!(co_await stream.write_all(
          std::string_view{"12\r\nGET visits\nGET stock\nINCR stock\nDEL "
                           "stock\nGET stock\nSET bad nope\nUNKNOWN\nQUIT\n"})))
    throw std::runtime_error("流水线请求发送失败");
  faio::io::BufReader reader{stream};
  // 两个并发客户端各自递增五次，最终必须等于 10，不能发生共享状态更新丢失。
  for (const std::string_view expected :
       {"OK\n", "VALUE 10\n", "VALUE 12\n", "VALUE 13\n", "DELETED\n",
        "NOT_FOUND\n", "ERR integer\n", "ERR command\n", "BYE\n"}) {
    std::string response;
    if (!(co_await faio::io::read_line(reader, response, 128)) ||
        response != expected)
      throw std::runtime_error("协议校验失败，实际响应：" + response);
  }
}

faio::task<void> client_workflow(faio::net::address address) {
  // 两个并发连接统计访问量，随后第三个连接查询总量并操作库存。
  co_await faio::join(increment_client(address), increment_client(address));
  co_await verify_client(address);
}

faio::task<void> example_tcp_counter_self_test(counter_store &store) {
  const faio::net::address address{faio::net::v4addr{127, 0, 0, 1}, 0};
  auto bound = faio::net::TcpListener::bind(address);
  if (!bound)
    throw std::runtime_error("演示监听器绑定失败");
  auto listener = std::move(*bound);
  const auto local = listener.local_addr();
  if (!local)
    throw std::runtime_error("读取演示端口失败");
  // 临时端口避免和手动启动的服务冲突。join 返回之前服务及全部连接已经结束。
  co_await faio::join(accept_clients(listener, store, 3),
                      client_workflow(*local));
  faio::log::logger()->info(
      "TCP 应用场景通过：并发计数=10，拆分请求、流水线和错误处理正常");
}
} // namespace

int main(int argc, char **argv) {
  try {
    bool self_test = false;
    std::vector<char *> backend_arguments{argv[0]};
    for (int i = 1; i < argc; ++i) {
      if (std::string_view{argv[i]} == "--self-test")
        self_test = true;
      else
        backend_arguments.push_back(argv[i]);
    }
    // 后端参数在本文件内处理，示例不依赖其他 example 的工具函数。
    auto builder = faio::config_builder{};
#if defined(__linux__)
    if (backend_arguments.size() > 2)
      throw std::invalid_argument("仅支持 --io-backend=epoll|uring");
    std::string_view selection;
    if (backend_arguments.size() == 2) {
      constexpr std::string_view prefix{"--io-backend="};
      const std::string_view argument{backend_arguments[1]};
      if (!argument.starts_with(prefix) || argument.size() == prefix.size())
        throw std::invalid_argument("请使用 --io-backend=epoll|uring");
      selection = argument.substr(prefix.size());
    } else if (const char *environment = std::getenv("FAIO_TEST_IO_BACKEND")) {
      selection = environment;
    }
    if (selection == "epoll")
      builder.set_io_backend(faio::runtime::io_backend::IO_EPOLL);
    else if (selection == "uring")
      builder.set_io_backend(faio::runtime::io_backend::IO_URING);
    else if (!selection.empty())
      throw std::invalid_argument("IO 后端必须是 epoll 或 uring");
#else
    (void)backend_arguments;
    if (backend_arguments.size() > 1)
      throw std::invalid_argument(
          "本平台使用固定 IO 后端，无需选择 Linux 后端");
#endif
    faio::runtime::configure(builder.set_num_workers(4).build());
    // store 位于 block_on 外，保证所有连接（包括异常退出时排空的后台连接）
    // 使用它时对象仍存活。单个请求持锁处理，网络等待发生在锁外。
    counter_store store;
    if (self_test)
      faio::block_on(example_tcp_counter_self_test(store));
    else
      faio::block_on(example_tcp_counter_server(store));
    faio::runtime::shutdown();
  } catch (const std::exception &error) {
    faio::log::logger()->error("TCP 应用失败：{}", error.what());
    return 1;
  }
}
