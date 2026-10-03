#include "backend_test_support.hpp"
/**
 * @file test_unix_contract.cpp
 * @brief Unix stream/datagram、split/reunite、匿名 pipe/FIFO 的跨后端契约。
 */
#include "test_support.hpp"
#include <array>
#include <chrono>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <sys/stat.h>
#include <sys/uio.h>

namespace {
using faio_test::take;
using namespace std::chrono_literals;

/** @brief owned 写半边析构半关闭、读半边继续存活；错误重合不消费半边。 */
faio::task<bool> stream_half_lifetime() {
  auto endpoints = take(faio::net::unix::UnixStream::pair());
  const auto credentials = take(endpoints.first.peer_credentials());
  if (credentials.uid != ::getuid() || credentials.gid != ::getgid())
    co_return false;
  auto halves = std::move(endpoints.first).into_split();
  {
    auto writer = std::move(halves.second);
    take(co_await writer.write_all(std::span<const char>{"half", 4}));
  }
  std::array<char, 4> data{};
  take(co_await endpoints.second.read_exact(data));
  if (std::string_view(data.data(), 4) != "half")
    co_return false;
  if (take(co_await endpoints.second.read(data).set_timeout(100ms)) != 0)
    co_return false;
  take(co_await endpoints.second.write_all(std::span<const char>{"back", 4}));
  take(co_await halves.first.read_exact(data));
  if (std::string_view(data.data(), 4) != "back")
    co_return false;
  auto first = take(faio::net::unix::UnixStream::pair());
  auto second = take(faio::net::unix::UnixStream::pair());
  auto left = std::move(first.first).into_split();
  auto right = std::move(second.first).into_split();
  const auto mismatch = std::move(left.first).reunite(std::move(right.second));
  if (mismatch || mismatch.error().value() != faio::Error::ReuniteFailed)
    co_return false;
  auto combined = take(std::move(left.first).reunite(std::move(left.second)));
  take(co_await combined.write_all(std::span<const char>{"join", 4}));
  take(co_await first.second.read_exact(data));
  co_return std::string_view(data.data(), 4) == "join";
}

/** @brief 拥有型操作在构造时保存资源租约；原 wrapper 移动不影响延迟 await。 */
faio::task<bool> delayed_owned_operation_after_move() {
  auto endpoints = take(faio::net::unix::UnixStream::pair());
  auto pending = endpoints.first.read(faio::io::io_buffer{4});
  auto moved = std::move(endpoints.first);
  take(co_await endpoints.second.write_all(std::span<const char>{"move", 4}));
  auto result = take(co_await std::move(pending));
  if (result.bytes != 4 ||
      std::string_view(result.buffer.data(), result.buffer.size()) != "move")
    co_return false;
  auto writing =
      moved.write(faio::io::io_buffer::copy(std::string_view{"back"}));
  auto moved_again = std::move(moved);
  if (take(co_await std::move(writing)).bytes != 4)
    co_return false;
  std::array<char, 4> buffer{};
  take(co_await endpoints.second.read_exact(buffer));
  co_return std::string_view(buffer.data(), 4) == "back";
}

faio::task<void> unix_echo(faio::net::unix::UnixListener &listener) {
  auto accepted = take(co_await listener.accept());
  std::array<char, 4> bytes{};
  take(co_await accepted.first.read_exact(bytes));
  take(co_await accepted.first.write_all(bytes));
}
/** @brief pathname bind/accept/connect 以及配置 socket 的转换都运行实际 IO。 */
faio::task<bool> unix_listener(const std::filesystem::path &path) {
  const auto address = take(faio::net::unix::address::pathname(path.string()));
  auto configurable = take(faio::net::unix::UnixSocket::stream());
  take(configurable.bind(address));
  auto listener = take(std::move(configurable).listen());
  auto server = faio::spawn(unix_echo(listener));
  auto socket = take(faio::net::unix::UnixSocket::stream());
  auto stream = take(co_await std::move(socket).connect(address));
  take(co_await stream.write_all(std::span<const char>{"unix", 4}));
  std::array<char, 4> bytes{};
  take(co_await stream.read_exact(bytes));
  co_await server;
  co_return std::string_view(bytes.data(), 4) == "unix";
}

/** @brief Unix datagram 保留空包与消息边界；pathname 地址可返回发送者地址。 */
faio::task<bool> unix_datagrams(const std::filesystem::path &root) {
  auto paired = take(faio::net::unix::UnixDatagram::pair());
  take(co_await paired.first.send(std::span<const char>{}));
  std::array<char, 8> buffer{};
  if (take(co_await paired.second.recv(buffer)) != 0)
    co_return false;
  take(co_await paired.first.send(std::span<const char>{"packet", 6}));
  if (take(co_await paired.second.peek(buffer)) != 6 ||
      take(co_await paired.second.recv(buffer)) != 6)
    co_return false;
  auto first = take(faio::net::unix::UnixDatagram::bind(
      take(faio::net::unix::address::parse((root / "sender").string()))));
  auto second = take(faio::net::unix::UnixDatagram::bind(
      take(faio::net::unix::address::parse((root / "receiver").string()))));
  const auto local = take(second.local_addr());
  take(co_await first.send_to(std::span<const char>{"path", 4}, local));
  const auto received = take(co_await second.recv_from(buffer));
  co_return received.first == 4 &&
      received.second.path() == (root / "sender").string() &&
      std::string_view(buffer.data(), 4) == "path";
}

/** @brief pipe 读写和 EOF、EPIPE；库须局部抑制
 * SIGPIPE，不能修改进程全局处理器。 */
faio::task<bool> pipe_roundtrip_and_errors() {
  auto endpoints = take(faio::net::unix::pipe::pair());
  take(co_await endpoints.first.write_all(std::span<const char>{"pipe", 4}));
  take(co_await endpoints.first.close());
  std::array<char, 4> bytes{};
  take(co_await endpoints.second.read_exact(bytes));
  if (take(co_await endpoints.second.read(bytes)) != 0 ||
      std::string_view(bytes.data(), 4) != "pipe")
    co_return false;
  auto broken = take(faio::net::unix::pipe::pair());
  take(co_await broken.second.close());
  const auto failed =
      co_await broken.first.write(std::span<const char>{"x", 1});
  co_return !failed && failed.error().value() == EPIPE;
}

/** @brief 已满pipe的WRITE/WRITEV先挂起，随后读端关闭不能产生进程级SIGPIPE。 */
faio::task<faio::expected<std::size_t>>
suspended_pipe_write(faio::io::detail::resource_ptr resource, bool vectored) {
  const std::array<char, 4> payload{'f', 'u', 'l', 'l'};
  if (!vectored)
    co_return co_await faio::io::write(
        std::move(resource), payload.data(), payload.size(),
        std::numeric_limits<std::uint64_t>::max())
        .set_timeout(1s);
  const std::array<iovec, 2> vectors{
      {{const_cast<char *>(payload.data()), 2},
       {const_cast<char *>(payload.data() + 2), 2}}};
  co_return co_await faio::io::writev(std::move(resource), vectors.data(),
                                      vectors.size(),
                                      std::numeric_limits<std::uint64_t>::max())
      .set_timeout(1s);
}
faio::task<bool> full_pipe_reader_closes() {
  for (const bool vectored : {false, true}) {
    auto endpoints = take(faio::net::unix::pipe::pair());
    std::array<char, 4096> fill{};
    while (::write(endpoints.first.fd(), fill.data(), fill.size()) > 0) {
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      throw std::system_error(errno, std::generic_category());
    auto pending =
        faio::spawn(suspended_pipe_write(endpoints.first.resource(), vectored));
    co_await faio::time::sleep(3ms);
    if (pending.done()) {
      (void)co_await pending;
      co_return false;
    }
    take(co_await endpoints.second.close());
    const auto result = co_await pending;
    if (result || result.error().value() != EPIPE)
      co_return false;
  }
  co_return true;
}

/** @brief 已打开非阻塞 FIFO、异步打开与原生句柄导出/再导入。 */
faio::task<bool> fifo_and_native(const std::filesystem::path &filename) {
  auto context = faio::io::io_context::current();
  if (::mkfifo(filename.c_str(), 0600))
    throw std::system_error(errno, std::generic_category());
  const auto before_open = context.statistics();
  auto reader = take(co_await faio::net::unix::pipe::Receiver::open(
      context, filename.string()));
  auto sender = take(
      co_await faio::net::unix::pipe::Sender::open(context, filename.string()));
  const auto after_open = context.statistics();
  if (context.domain()->capabilities().native_filesystem &&
      (after_open.native_completed < before_open.native_completed + 2 ||
       after_open.native_submitted < before_open.native_submitted + 2 ||
       after_open.native_flushed < before_open.native_flushed + 2 ||
       context.blocking().started_threads() != 0 ||
       context.resolver().started_threads() != 0 ||
       context.cleanup().started_threads() != 0))
    co_return false; // 两次异步 FIFO open 都必须拥有真实 OPENAT SQE/CQE。
  auto native = take(sender.into_native());
  auto imported = take(
      faio::net::unix::pipe::Sender::from_native(context, std::move(native)));
  take(co_await imported.write_all(std::span<const char>{"fifo", 4}));
  std::array<char, 4> bytes{};
  take(co_await reader.read_exact(bytes));
  take(co_await imported.close());
  co_return take(co_await reader.read(bytes)) == 0 &&
      std::string_view(bytes.data(), 4) == "fifo";
}
} // namespace

class UnixContract : public testing::TestWithParam<faio::runtime::mode> {
protected:
  auto config() const {
    return faio_test::config_builder()
        .set_mode(GetParam())
        .set_num_workers(4)
        .build();
  }
};
TEST_P(UnixContract, OwnedSplitHalfCloseCredentialsAndReunite) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(stream_half_lifetime()));
}
TEST_P(UnixContract, DelayedOwnedOperationsRetainResourceAcrossWrapperMove) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(delayed_owned_operation_after_move()));
}
TEST_P(UnixContract, PathnameConfiguredListenerAndStreamConnect) {
  faio_test::temporary_directory root;
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(unix_listener(root.path() / "socket")));
}
TEST_P(UnixContract, DatagramPairEmptyPeekAndPathnameAddress) {
  faio_test::temporary_directory root;
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(unix_datagrams(root.path())));
}
TEST_P(UnixContract, PipeEofAndBrokenPipeAreOrdinaryResults) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(pipe_roundtrip_and_errors()));
}
TEST_P(UnixContract,
       FullPipeWriteAndWritevReturnEpipeWhenReaderClosesAfterSuspension) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(full_pipe_reader_closes()));
}
TEST_P(UnixContract, FifoOpenAndNativeInterop) {
  faio_test::temporary_directory root;
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(fifo_and_native(root.path() / "fifo")));
}
INSTANTIATE_TEST_SUITE_P(RuntimeModes, UnixContract,
                         testing::Values(faio::runtime::mode::current_thread,
                                         faio::runtime::mode::multi_thread));
