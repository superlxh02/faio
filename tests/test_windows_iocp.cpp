/** @file test_windows_iocp.cpp @brief 真实内核完成协议、取消和稳定槽复用的
 * Windows 合同。 */
#include <array>
#include <chrono>
#include <faio/faio.hpp>
#include <gtest/gtest.h>
#include <thread>

namespace {
using namespace faio::io;
using namespace faio::io::detail;

struct socket_pair {
  faio::io::windows::owned_socket_handle reader, writer;

  socket_pair() {
    if (!windows::initialize_winsock())
      throw std::runtime_error("WSAStartup");
    windows::owned_socket_handle listener{
        ::WSASocketW(AF_INET, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED)};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address))
        || ::listen(listener.get(), 8))
      throw std::runtime_error("listen");
    int length = sizeof(address);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length))
      throw std::runtime_error("getsockname");
    writer.reset(::WSASocketW(AF_INET, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED));
    if (::connect(writer.get(), reinterpret_cast<sockaddr*>(&address), length))
      throw std::runtime_error("connect");
    reader.reset(::accept(listener.get(), nullptr, nullptr));
    if (!reader || !writer)
      throw std::runtime_error("accept");
    if (!windows::prepare_socket(static_cast<native_descriptor>(reader.get()))
        || !windows::prepare_socket(static_cast<native_descriptor>(writer.get())))
      throw std::runtime_error("prepare socket");
  }
};

struct observed {
  unsigned count{};
  std::int64_t result{};
  std::uint64_t progress{};

  completion_target target() {
    return {this, +[](void* pointer, std::int64_t result, std::uint64_t progress) noexcept {
              auto& value = *static_cast<observed*>(pointer);
              ++value.count;
              value.result = result;
              value.progress = progress;
            }};
  }
};

void drain_until(io_engine& engine, const observed& completion) {
  auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!completion.count && std::chrono::steady_clock::now() < limit)
    engine.driver().wait_and_drive(10);
}

TEST(WindowsIocp, PendingCancellationKeepsSlotUntilOriginalCompletionAndRejectsStaleToken) {
  socket_pair sockets;
  engine_config config;
  config.max_operations = 1;
  io_engine engine{config};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(static_cast<native_descriptor>(sockets.reader.get()), false);
  char byte{};
  io_request request;
  request.kind = operation_kind::recv;
  request.resource = resource;
  request.buffer = &byte;
  request.length = 1;
  observed first;
  int error{};
  auto token = domain->prepare_submit(std::move(request), first.target(), error);
  ASSERT_NE(token.value, 0u);
  EXPECT_EQ(first.count, 0u);
  engine.submitter().request_cancel(token);
  EXPECT_EQ(first.count, 0u);
  EXPECT_FALSE(engine.driver().quiescent());  // CancelIoEx ACK 绝不能回收借用。
  io_request rejected;
  rejected.kind = operation_kind::recv;
  rejected.resource = resource;
  rejected.buffer = &byte;
  rejected.length = 1;
  observed second;
  EXPECT_EQ(domain->prepare(std::move(rejected), second.target(), error).value, 0u);
  EXPECT_EQ(error, EAGAIN);
  drain_until(engine, first);
  ASSERT_EQ(first.count, 1u);
  EXPECT_EQ(first.result, -ECANCELED);
  io_request next;
  next.kind = operation_kind::recv;
  next.resource = resource;
  next.buffer = &byte;
  next.length = 1;
  auto newer = domain->prepare_submit(std::move(next), second.target(), error);
  ASSERT_NE(newer.value, 0u);
  EXPECT_NE(newer.value, token.value);
  engine.submitter().request_cancel(token);  // 老 token 不得取消刚复用的地址。
  ASSERT_EQ(::send(sockets.writer.get(), "q", 1, 0), 1);
  drain_until(engine, second);
  EXPECT_EQ(second.count, 1u);
  EXPECT_EQ(second.result, 1);
  EXPECT_EQ(byte, 'q');
  engine.driver().drive();
  EXPECT_EQ(first.count, 1u);
  EXPECT_EQ(second.count, 1u);
  EXPECT_EQ(engine.statistics().native_submitted, engine.statistics().native_completed);
}

/** @brief 非磁盘 HANDLE 默认读必须保留原生取消，不占文件服务永久等待。 */
TEST(WindowsIocp, PendingPipeHandleReadCancelsAndDrainsBeforeSlotReuse) {
  const auto name = L"\\\\.\\pipe\\faio_iocp_" + std::to_wstring(::GetCurrentProcessId()) + L"_"
                    + std::to_wstring(::GetTickCount64());
  windows::owned_file_handle server{
      ::CreateNamedPipeW(name.c_str(),
                         PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                         1,
                         4096,
                         4096,
                         0,
                         nullptr)};
  ASSERT_TRUE(server);
  windows::owned_file_handle client{::CreateFileW(
      name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  ASSERT_TRUE(client);
  OVERLAPPED connect{};
  if (!::ConnectNamedPipe(server.get(), &connect))
    ASSERT_EQ(::GetLastError(), ERROR_PIPE_CONNECTED);
  engine_config config;
  config.max_operations = 1;
  io_engine engine{config};
  auto domain = engine.context().domain();
  char byte{};
  auto request = [&] {
    io_request value;
    value.kind = operation_kind::read;
    value.native_kind = native_handle_kind::windows_handle;
    value.bypass_resource_registration = true;
    value.windows_implicit_cursor = true;  // HANDLE 管道没有 seek 游标，不应进入磁盘游标 lane。
    value.fd = reinterpret_cast<native_descriptor>(server.get());
    value.buffer = &byte;
    value.length = 1;
    return value;
  };
  observed first;
  int error{};
  const auto old_token = domain->prepare_submit(request(), first.target(), error);
  ASSERT_NE(old_token.value, 0u);
  EXPECT_EQ(first.count, 0u);
  engine.submitter().request_cancel(old_token);
  EXPECT_EQ(first.count, 0u);  // CancelIoEx ACK 不能提前释放借用缓冲区或稳定槽。
  drain_until(engine, first);
  ASSERT_EQ(first.count, 1u);
  EXPECT_EQ(first.result, -ECANCELED);
  observed second;
  const auto next_token = domain->prepare_submit(request(), second.target(), error);
  ASSERT_NE(next_token.value, 0u);
  ASSERT_NE(next_token.value, old_token.value);
  engine.submitter().request_cancel(old_token);  // 完整代际阻止旧取消作用于新的 HANDLE 读。
  DWORD written{};
  ASSERT_TRUE(::WriteFile(client.get(), "p", 1, &written, nullptr));
  ASSERT_EQ(written, 1u);
  drain_until(engine, second);
  ASSERT_EQ(second.count, 1u);
  EXPECT_EQ(second.result, 1);
  EXPECT_EQ(byte, 'p');
  engine.shutdown();
  EXPECT_TRUE(engine.driver().quiescent());
  EXPECT_EQ(engine.statistics().native_submitted, engine.statistics().native_completed);
}

TEST(WindowsIocp, SynchronousSuccessAndKernelCompletionPublishExactlyOnce) {
  socket_pair sockets;
  io_engine engine;
  auto domain = engine.context().domain();
  auto resource = domain->adopt(static_cast<native_descriptor>(sockets.reader.get()), false);
  for (unsigned i = 0; i < 64; ++i) {
    ASSERT_EQ(::send(sockets.writer.get(), "x", 1, 0), 1);
    char byte{};
    io_request request;
    request.kind = operation_kind::recv;
    request.resource = resource;
    request.buffer = &byte;
    request.length = 1;
    observed complete;
    int error{};
    ASSERT_NE(domain->prepare_submit(std::move(request), complete.target(), error).value, 0u);
    drain_until(engine, complete);
    EXPECT_EQ(complete.count, 1u);
    EXPECT_EQ(complete.result, 1);
    EXPECT_EQ(byte, 'x');
    for (int extra = 0; extra < 3; ++extra)
      engine.driver().drive();
    EXPECT_EQ(complete.count, 1u);
  }
  EXPECT_EQ(engine.statistics().native_submitted, 64u);
  EXPECT_EQ(engine.statistics().native_completed, 64u);
}

TEST(WindowsIocp, MultipleNonConsumingObserversAndTimeoutDrain) {
  socket_pair sockets;
  io_engine engine;
  auto domain = engine.context().domain();
  auto resource = domain->adopt(static_cast<native_descriptor>(sockets.reader.get()), false);
  observed left, right;
  int error{};
  for (auto* completion : {&left, &right}) {
    io_request request;
    request.kind = operation_kind::ready;
    request.resource = resource;
    request.argument = 1;
    ASSERT_NE(domain->prepare_submit(std::move(request), completion->target(), error).value, 0u);
  }
  ASSERT_EQ(::send(sockets.writer.get(), "r", 1, 0), 1);
  drain_until(engine, left);
  drain_until(engine, right);
  EXPECT_EQ(left.count, 1u);
  EXPECT_EQ(right.count, 1u);
  char byte{};
  EXPECT_EQ(::recv(sockets.reader.get(), &byte, 1, 0), 1);
  EXPECT_EQ(byte, 'r');  // 观察不消费。
  observed timeout;
  io_request request;
  request.kind = operation_kind::recv;
  request.resource = resource;
  request.buffer = &byte;
  request.length = 1;
  request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
  ASSERT_NE(domain->prepare_submit(std::move(request), timeout.target(), error).value, 0u);
  drain_until(engine, timeout);
  EXPECT_EQ(timeout.count, 1u);
  EXPECT_EQ(timeout.result, -ETIMEDOUT);
  EXPECT_TRUE(engine.driver().quiescent());
}

TEST(WindowsIocp, RemoteWakeAndFullPoolBackpressureHaveNoLostCompletion) {
  windows::iocp_backend backend{1};
  socket_pair sockets;
  char first{}, second{};
  io_request a, b;
  a.kind = b.kind = operation_kind::recv;
  a.fd = b.fd = static_cast<native_descriptor>(sockets.reader.get());
  a.buffer = &first;
  b.buffer = &second;
  a.length = b.length = 1;
  ASSERT_EQ(backend.try_submit({1, &a}).status, backend_submit_status::accepted);
  EXPECT_EQ(backend.try_submit({2, &b}).status, backend_submit_status::would_queue);
  std::thread cancel([&] {
    for (unsigned i = 0; i < 1000; ++i)
      backend.wake();
    backend.request_cancel({1, &a});
  });
  std::array<backend_event, 2> events;
  unsigned results{};
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!results && std::chrono::steady_clock::now() < deadline) {
    int count = backend.poll(events, 10);
    ASSERT_GE(count, 0);
    for (int i = 0; i < count; ++i)
      if (events[static_cast<std::size_t>(i)].key == 1)
        ++results;
  }
  cancel.join();
  EXPECT_EQ(results, 1u);
  EXPECT_TRUE(backend.quiescent());
}

faio::task<void> raw_file_contract(std::filesystem::path directory) {
  auto path = (directory / L"原生低层文件.bin").u8string();
  auto opened = co_await faio::io::open(reinterpret_cast<const char*>(path.c_str()),
                                        _O_CREAT | _O_TRUNC | _O_RDWR);
  if (!opened)
    throw std::runtime_error("raw open failed");
  HANDLE handle = *opened;
  const std::array<char, 3> content{'a', 'b', 'c'};
  auto wrote = co_await faio::io::write(handle, content.data(), content.size(), 0);
  EXPECT_TRUE(wrote);
  if (wrote)
    EXPECT_EQ(*wrote, 3u);
  std::array<char, 3> result{};
  auto read = co_await faio::io::read(handle, result.data(), result.size(), 0);
  EXPECT_TRUE(read);
  EXPECT_EQ(result, content);
  std::array<iovec, 2> vectors{{{result.data(), 1}, {result.data() + 1, 2}}};
  auto vector_write = co_await faio::io::writev(handle, vectors.data(), 2, 3);
  EXPECT_TRUE(vector_write);
  if (vector_write)
    EXPECT_EQ(*vector_write, 3u);
  result = {};
  auto vector_read = co_await faio::io::readv(handle, vectors.data(), 2, 3);
  EXPECT_TRUE(vector_read);
  EXPECT_EQ(result, content);
  auto synced = co_await faio::io::fsync(handle);
  EXPECT_TRUE(synced);
  auto closed = co_await faio::io::close(handle);
  EXPECT_TRUE(closed);
  windows::owned_file_handle parent{
      ::CreateFileW(directory.c_str(),
                    FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr)};
  if (!parent)
    throw std::runtime_error("raw directory handle failed");
  auto child = co_await faio::io::openat(parent.get(), "child.bin", _O_CREAT | _O_RDWR);
  if (!child)
    throw std::runtime_error("raw openat failed");
  EXPECT_TRUE(co_await faio::io::close(*child));
  auto context = faio::io::current_context();
  EXPECT_EQ(context.statistics().native_submitted, context.statistics().native_completed);
  co_return;
}

TEST(WindowsIocp, RawFileOpenReadWriteVectoredFlushRelativeOpenAndClose) {
  auto directory = std::filesystem::temp_directory_path() / L"faio_windows_raw_iocp";
  std::filesystem::create_directories(directory);
  faio::runtime::detail::runtime_config config;
  config._mode = faio::runtime::mode::current_thread;
  faio::runtime::detail::runtime_context runtime{config};
  runtime.block_on(raw_file_contract(directory));
  std::filesystem::remove_all(directory);
}
}  // namespace
