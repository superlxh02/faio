#include "backend_test_support.hpp"
/**
 * @file test_io_lifecycle.cpp
 * @brief 真实后端下的 IO 截止时间、取消、关闭、fd 复用和独占方向协议。
 * @details socketpair 避免外部服务依赖；wrapper 的共享资源绑定创建时 domain。
 */
#include "test_support.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <latch>
#include <optional>
#include <sys/socket.h>
#include <thread>

namespace {
using faio_test::take;
using namespace std::chrono_literals;

/** @brief 原生 socketpair 在导入资源控制块后转移给 Socket 所有者。 */
std::pair<faio::net::detail::Socket, faio::net::detail::Socket> make_pair() {
  std::array<int, 2> descriptors{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()))
    throw std::system_error(errno, std::generic_category());
  faio::net::detail::owned_native_socket first{descriptors[0]}, second{descriptors[1]};
  take(faio::net::detail::Socket::prepare(first.get()));
  take(faio::net::detail::Socket::prepare(second.get()));
  faio::net::detail::Socket left{first.get()};
  (void)first.release();
  faio::net::detail::Socket right{second.get()};
  (void)second.release();
  return {std::move(left), std::move(right)};
}

faio::task<int> pending_read(faio::io::detail::resource_ptr resource,
                             std::atomic<unsigned>& resumed) {
  char value{};
  auto result = co_await faio::io::recv(std::move(resource), &value, 1, 0);
  resumed.fetch_add(1, std::memory_order_relaxed);
  co_return result ? static_cast<int>(*result) : -result.error().value();
}

/** @brief timeout 返回前已经撤销借用；下一次读能消费随后送来的数据。 */
faio::task<bool> deadline_and_reuse() {
  auto pair = make_pair();
  char value{};
  const auto timed_out =
      co_await faio::io::recv(pair.first.resource(), &value, 1, 0).set_timeout(3ms);
  if (timed_out || timed_out.error().value() != ETIMEDOUT)
    co_return false;
  const char sent = 't';
  if (take(co_await faio::io::send(pair.second.resource(), &sent, 1, 0)) != 1)
    co_return false;
  const auto received =
      co_await faio::io::recv(pair.first.resource(), &value, 1, 0).set_timeout(100ms);
  co_return received && *received == 1 && value == sent;
}

/** @brief 同方向冲突拒绝；close 取消所有已接受读并禁止 fd 导出时遗失操作。 */
faio::task<bool> conflict_cancel_close() {
  auto pair = make_pair();
  std::atomic<unsigned> resumed{0};
  auto pending = faio::spawn(pending_read(pair.first.resource(), resumed));
  // 驱动至少两个事件轮，使挂起 IO 进入 reactor 的长期注册。
  co_await faio::time::sleep(3ms);
  char byte{};
  auto empty_conflict =
      co_await faio::io::recv(pair.first.resource(), &byte, 0, 0).empty_success().set_timeout(20ms);
  if (empty_conflict || empty_conflict.error().value() != EBUSY) {
    pending.request_stop();
    (void)co_await pending;
    co_return false;
  }
  auto conflict = co_await faio::io::recv(pair.first.resource(), &byte, 1, 0);
  if (conflict || conflict.error().value() != EBUSY) {
    pending.request_stop();
    (void)co_await pending;
    co_return false;
  }
  auto exporting = pair.first.into_native();
  if (exporting || exporting.error().value() != EBUSY) {
    pending.request_stop();
    (void)co_await pending;
    co_return false;
  }
  take(co_await pair.first.close());
  if (co_await pending != -ECANCELED || resumed.load() != 1)
    co_return false;
  auto closed = co_await faio::io::recv(pair.first.resource(), &byte, 1, 0);
  co_return !closed && closed.error().value() == EBADF;
}

/** @brief stop 可在 await_suspend 前触发，也可在真正挂起后触发。 */
faio::task<bool> repeated_cancel() {
  for (int iteration = 0; iteration < 100; ++iteration) {
    auto pair = make_pair();
    std::atomic<unsigned> resumed{0};
    auto pending = faio::spawn(pending_read(pair.first.resource(), resumed));
    if (iteration % 2)
      co_await faio::time::sleep(1ms);
    pending.request_stop();
    if (co_await pending != -ECANCELED || resumed.load() != 1)
      co_return false;
    const char sent = 'c';
    take(co_await faio::io::send(pair.second.resource(), &sent, 1, 0));
    char actual{};
    if (take(co_await faio::io::recv(pair.first.resource(), &actual, 1, 0).set_timeout(100ms)) != 1
        || actual != sent)
      co_return false;
  }
  co_return true;
}

/** @brief 原cancel flags兼容资源取消；控制完成后必须等原操作drain才能复用借用。
 * @details 0、ALL(1)、FD(2)、ALL|FD(3)用于稳定资源；ANY(4)、FIXED(8)
 *          不在公开资源模型内，必须明确拒绝且不能影响原pending操作。
 */
faio::task<bool> literal_cancel_flags_drain_original_operation() {
  for (unsigned flags = 0; flags != 4; ++flags) {
    auto endpoints = make_pair();
    std::atomic<unsigned> resumed{0};
    auto pending = faio::spawn(pending_read(endpoints.first.resource(), resumed));
    co_await faio::time::sleep(3ms);
    bool valid = !pending.done() && resumed.load() == 0;
    for (const unsigned unsupported : {4u, 8u}) {
      const auto rejected = co_await faio::io::cancel(endpoints.first.resource(), unsupported);
      valid = valid && !rejected && rejected.error().value() == ENOTSUP && !pending.done()
              && resumed.load() == 0;
    }
    // raw fd与现代resource重载必须选中同一个generation，不能取消后来复用的fd。
    const auto control = flags < 2 ? co_await faio::io::cancel(endpoints.first.resource(), flags)
                                   : co_await faio::io::cancel(endpoints.first.fd(), flags);
    valid = valid && control.has_value();
    if (!valid)
      pending.request_stop();  // 失败路径也回收协程，正常路径只能由flags取消完成。
    const auto original = co_await pending;
    valid = valid && original == -ECANCELED && resumed.load() == 1;
    const char sent = 'f';
    char received{};
    valid = valid && take(co_await faio::io::send(endpoints.second.resource(), &sent, 1, 0)) == 1;
    valid = valid
            && take(co_await faio::io::recv(endpoints.first.resource(), &received, 1, 0)
                        .set_timeout(100ms))
                   == 1
            && received == sent;
    if (!valid)
      co_return false;
  }
  co_return true;
}

/** @brief 关闭并复用相同整数 fd，旧就绪事件不能恢复新资源上的错误操作。 */
faio::task<bool> descriptor_generation() {
  auto pair = make_pair();
  const int retired = pair.first.fd();
  const auto old_resource = pair.first.resource();
  const char old = 'o';
  take(co_await faio::io::send(pair.second.resource(), &old, 1, 0));
  (void)take(co_await faio::io::ready(pair.first.resource(), faio::io::Interest::readable));
  take(co_await pair.first.close());
  auto fresh = make_pair();
  auto native = take(fresh.first.into_native());
  if (native.get() != retired) {
    if (::dup2(native.get(), retired) < 0)
      throw std::system_error(errno, std::generic_category());
    native.reset();
    native = faio::net::detail::owned_native_socket{retired};
  }
  take(faio::net::detail::Socket::prepare(native.get()));
  faio::net::detail::Socket replacement{native.get()};
  (void)native.release();
  char actual{};
  auto stale = co_await faio::io::recv(old_resource, &actual, 1, 0);
  if (stale || stale.error().value() != EBADF)
    co_return false;
  auto empty = co_await faio::io::recv(replacement.resource(), &actual, 1, 0).set_timeout(3ms);
  if (empty || empty.error().value() != ETIMEDOUT)
    co_return false;
  const char new_value = 'n';
  take(co_await faio::io::send(fresh.second.resource(), &new_value, 1, 0));
  const auto result =
      co_await faio::io::recv(replacement.resource(), &actual, 1, 0).set_timeout(100ms);
  co_return result && *result == 1 && actual == new_value;
}

faio::task<std::pair<faio::net::unix::UnixStream, faio::net::unix::UnixStream>> owned_pair() {
  co_return take(faio::net::unix::UnixStream::pair());
}

faio::task<void> empty_task() {
  co_return;
}

/** @brief 外部停止源已经触发时，立即可读/可写也不能抢先产生 syscall 副作用。 */
faio::task<bool> cancelled_immediate_io(faio::io::detail::resource_ptr input,
                                        faio::io::detail::resource_ptr output,
                                        std::latch& entered,
                                        std::latch& released) {
  entered.count_down();
  released.wait();  // 外部线程先 request_stop，再解开屏障；不依赖调度先后顺序。
  char value{};
  const auto read = co_await faio::io::recv(std::move(input), &value, 1, 0);
  const char unwanted = 'x';
  const auto write = co_await faio::io::send(std::move(output), &unwanted, 1, 0);
  co_return !read && read.error().value() == ECANCELED && !write
      && write.error().value() == ECANCELED;
}

/** @brief 取消任务没有消费已就绪数据，也没有向另一端写入额外字节。 */
faio::task<bool> verify_cancelled_io_has_no_side_effect(
    std::pair<faio::net::unix::UnixStream, faio::net::unix::UnixStream>& endpoints) {
  char received{};
  const auto preserved =
      co_await endpoints.first.read(std::span<char>{&received, 1}).set_timeout(100ms);
  if (!preserved || *preserved != 1 || received != 'r')
    co_return false;
  const auto absent =
      co_await endpoints.second.read(std::span<char>{&received, 1}).set_timeout(3ms);
  co_return !absent && absent.error().value() == ETIMEDOUT;
}

/** @brief 写半关闭必须等待组合写方向租约，不能在两次native
 * write之间提前SHUT_WR。 */
faio::task<bool> half_close_waits_for_composite_reservation() {
  auto endpoints = make_pair();
  char owner{};
  {
    auto lease = take(faio::io::detail::reserve_direction(
        endpoints.first.resource(), faio::io::Interest::writable, &owner));
    endpoints.first.resource()->request_shutdown_write();
    const char sent = 'w';
    const auto written =
        co_await faio::io::send(endpoints.first.resource(), &sent, 1, 0).reservation(&owner);
    if (!written || *written != 1)
      co_return false;
    char received{};
    const auto reading =
        co_await faio::io::recv(endpoints.second.resource(), &received, 1, 0).set_timeout(100ms);
    if (!reading || *reading != 1 || received != sent)
      co_return false;
  }  // 最后写租约释放才执行 SHUT_WR；此前字节已完成。
  char byte{};
  const auto eof =
      co_await faio::io::recv(endpoints.second.resource(), &byte, 1, 0).set_timeout(100ms);
  co_return eof && *eof == 0;
}

/** @brief 停止先于首次 fused send，既不能提交 payload，也不能残留租约。 */
faio::task<bool> cancelled_fused_first_send(faio::io::detail::resource_ptr resource,
                                            std::latch& entered,
                                            std::latch& released) {
  char owner{};
  faio::io::detail::direction_lease lease{resource, faio::io::Interest::writable, &owner};
  entered.count_down();
  released.wait();  // 外部先请求停止再释放屏障，避免靠调度/时间猜测取消顺序。
  const char unwanted = 'x';
  auto result = co_await faio::io::send(resource, &unwanted, 1, 0).establish_reservation(&owner);
  co_return !result && result.error().value() == ECANCELED;
}

faio::task<bool> cancelled_fused_first_send_has_no_payload_and_can_reuse(
    faio::io::detail::resource_ptr output, faio::io::detail::resource_ptr input) {
  char received{};
  auto absent = co_await faio::io::recv(input, &received, 1, 0).set_timeout(3ms);
  if (absent || absent.error().value() != ETIMEDOUT)
    co_return false;
  const char sent = 'r';
  auto written = co_await faio::io::send(output, &sent, 1, 0).set_timeout(100ms);
  auto read = co_await faio::io::recv(input, &received, 1, 0).set_timeout(100ms);
  co_return written && *written == 1 && read && *read == 1 && received == sent;
}

/** @brief 首个 send 取得组合租约后，短操作间隙仍排他并延后 owned 写半关闭。 */
faio::task<bool> fused_reservation_keeps_half_close_order() {
  auto endpoints = make_pair();
  char owner{};
  const std::array<char, 2> sent{'f', 's'};
  {
    faio::io::detail::direction_lease lease{
        endpoints.first.resource(), faio::io::Interest::writable, &owner};
    auto first = co_await faio::io::send(endpoints.first.resource(), sent.data(), 1, 0)
                     .establish_reservation(&owner)
                     .set_timeout(100ms);
    if (!first || *first != 1)
      co_return false;
    endpoints.first.resource()->request_shutdown_write();
    // 已完成首操作但仍持有整个组合写的租约；未带 owner 的零写也不得越过 gate。
    auto conflicting =
        co_await faio::io::send(endpoints.first.resource(), sent.data(), 0, 0).empty_success();
    if (conflicting || conflicting.error().value() != EBUSY)
      co_return false;
    auto second = co_await faio::io::send(endpoints.first.resource(), sent.data() + 1, 1, 0)
                      .establish_reservation(&owner)
                      .set_timeout(100ms);
    if (!second || *second != 1)
      co_return false;
    std::array<char, 2> received{};
    std::size_t progress{};
    while (progress < received.size()) {
      auto n = co_await faio::io::recv(endpoints.second.resource(),
                                       received.data() + progress,
                                       received.size() - progress,
                                       0)
                   .set_timeout(100ms);
      if (!n || *n == 0)
        co_return false;
      progress += *n;
    }
    if (received != sent)
      co_return false;
  }  // 租约统一退出后才允许 SHUT_WR，下一次读应观察 payload 之后的 EOF。
  char byte{};
  auto eof = co_await faio::io::recv(endpoints.second.resource(), &byte, 1, 0).set_timeout(100ms);
  co_return eof && *eof == 0;
}

/** @brief 已过期的首 send 不能消费数据、残留方向租约或妨碍接下来的有效写。 */
faio::task<bool> fused_reservation_rejected_first_send_releases_direction() {
  auto endpoints = make_pair();
  const char sent = 'v';
  char owner{};
  {
    faio::io::detail::direction_lease lease{
        endpoints.first.resource(), faio::io::Interest::writable, &owner};
    auto invalid = co_await faio::io::send(endpoints.first.resource(), &sent, 1, 0)
                       .establish_reservation(nullptr);
    if (invalid || invalid.error().value() != EINVAL)
      co_return false;
    auto expired = co_await faio::io::send(endpoints.first.resource(), &sent, 1, 0)
                       .establish_reservation(&owner)
                       .set_timeout_at(std::chrono::steady_clock::now() - 1ms);
    if (expired || expired.error().value() != ETIMEDOUT)
      co_return false;
    // 拒绝发生在取得租约前，因此独立单写现在就能执行，不必等无操作 lease 析构。
    auto valid =
        co_await faio::io::send(endpoints.first.resource(), &sent, 1, 0).set_timeout(100ms);
    if (!valid || *valid != 1)
      co_return false;
  }
  char received{};
  auto read =
      co_await faio::io::recv(endpoints.second.resource(), &received, 1, 0).set_timeout(100ms);
  co_return read && *read == 1 && received == sent;
}

/** @brief runtime 外创建的资源在首次组合 await 绑定到当前 domain。 */
faio::task<bool> externally_created_composite(
    std::pair<faio::net::unix::UnixStream, faio::net::unix::UnixStream>& pair) {
  const std::array<char, 4> expected{'b', 'i', 'n', 'd'};
  take(co_await pair.second.write_all(expected));
  std::array<char, 4> received{};
  take(co_await pair.first.read_exact(received));
  co_return received == expected&& pair.first.context() && pair.second.context();
}

/** @brief 公开 write_all 的帧参数拥有资源，wrapper 在首次 bind 前销毁仍可完成。
 */
faio::task<bool> complete_write_after_original_wrapper_destruction(
    faio::task<faio::expected<void>> writing,
    faio::net::unix::UnixStream& receiver,
    const std::array<char, 4>& expected) {
  const auto written = co_await std::move(writing);
  if (!written)
    co_return false;
  // 成功恢复可早于 publisher 返回与稳定槽回收；不要求此刻所有资源租约已归还。
  std::array<char, 4> received{};
  std::size_t progress{};
  while (progress < received.size()) {
    const auto count =
        co_await faio::io::recv(
            receiver.resource(), received.data() + progress, received.size() - progress, 0)
            .set_timeout(100ms);
    if (!count || !*count)
      co_return false;
    progress += *count;
  }
  char byte{};
  const auto eof = co_await faio::io::recv(receiver.resource(), &byte, 1, 0).set_timeout(100ms);
  co_return received == expected && eof && *eof == 0;
}

/** @brief 保留原uring command数值，typed helper与底层cmdsock拥有同一返回语义。
 */
faio::task<bool> socket_commands_preserve_native_values() {
  auto checked = [](const char* operation, faio::expected<std::size_t> result) {
    if (!result)
      throw std::runtime_error(std::string{operation} + ": "
                               + std::string{result.error().message()});
    return *result;
  };
  auto socket = take(faio::net::detail::Socket::create(AF_INET, SOCK_STREAM, 0));
  int keepalive = 1;
  if (checked("literal SET3",
              co_await faio::io::cmdsock(
                  3, socket.fd(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)))
      != 0)
    co_return false;
  keepalive = 0;
  if (checked("literal GET2",
              co_await faio::io::cmdsock(
                  2, socket.fd(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)))
          != sizeof(keepalive)
      || keepalive == 0)
    co_return false;
  keepalive = 0;
  if (checked("typed SET",
              co_await faio::io::setsockopt(
                  socket.fd(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)))
      != 0)
    co_return false;
  keepalive = 1;
  if (checked("typed GET",
              co_await faio::io::getsockopt(
                  socket.fd(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)))
          != sizeof(keepalive)
      || keepalive != 0)
    co_return false;
  const auto negative_length =
      co_await faio::io::cmdsock(2, socket.fd(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, -1);
  if (negative_length || negative_length.error().value() != EINVAL)
    co_return false;
  const auto unknown = co_await faio::io::cmdsock(99, socket.fd(), 0, 0, nullptr, 0);
  if (unknown || (unknown.error().value() != ENOTSUP && unknown.error().value() != EOPNOTSUPP))
    co_return false;
  auto endpoints = make_pair();
  // Unix
  // socket不支持内核URING_CMD，但原数值GET/SET必须在接受前选用等价syscall。
  int unix_receive_buffer = 32768;
  if (checked("Unix SET3",
              co_await faio::io::cmdsock(3,
                                         endpoints.first.fd(),
                                         SOL_SOCKET,
                                         SO_RCVBUF,
                                         &unix_receive_buffer,
                                         sizeof(unix_receive_buffer)))
      != 0)
    co_return false;
  unix_receive_buffer = 0;
  if (checked("Unix GET2",
              co_await faio::io::cmdsock(2,
                                         endpoints.first.fd(),
                                         SOL_SOCKET,
                                         SO_RCVBUF,
                                         &unix_receive_buffer,
                                         sizeof(unix_receive_buffer)))
          != sizeof(unix_receive_buffer)
      || unix_receive_buffer < 32768)
    co_return false;
  constexpr std::array<char, 7> bytes{'c', 'o', 'm', 'm', 'a', 'n', 'd'};
  take(co_await faio::io::send(endpoints.second.resource(), bytes.data(), bytes.size(), 0));
  if (checked("Unix INQ0 with payload",
              co_await faio::io::cmdsock(0, endpoints.first.fd(), 0, 0, nullptr, 0))
      != bytes.size())
    co_return false;
  const auto queued_output = co_await faio::io::cmdsock(1, endpoints.second.fd(), 0, 0, nullptr, 0);
#if defined(__linux__)
  if (!queued_output || *queued_output < bytes.size())
    co_return false;
#else
  if (!queued_output && queued_output.error().value() != ENOTSUP
      && queued_output.error().value() != EOPNOTSUPP && queued_output.error().value() != ENOTTY)
    co_return false;
#endif
  std::array<char, 7> received{};
  if (take(co_await faio::io::recv(endpoints.first.resource(), received.data(), received.size(), 0))
          != bytes.size()
      || received != bytes)
    co_return false;
  co_return checked("Unix INQ0 after read",
                    co_await faio::io::cmdsock(0, endpoints.first.fd(), 0, 0, nullptr, 0))
      == 0;
}

faio::task<void> indefinitely_sleeping_task(std::atomic<bool>& entered,
                                            std::atomic<bool>& cancelled) {
  entered.store(true, std::memory_order_release);
  try {
    co_await faio::time::sleep(std::chrono::hours{1});
  } catch (const faio::operation_cancelled&) {
    cancelled.store(true, std::memory_order_release);
  }
}

/** @brief 自定义非阻塞 syscall 通过 guard 消费
 * readiness，并保留更新的事件代际。 */
faio::task<bool> async_fd_guards(const std::filesystem::path& name) {
  using faio::io::unix::AsyncFd;
  using faio::io::unix::OwnedFd;
  std::array<int, 2> descriptors{-1, -1};
  if (::pipe(descriptors.data()))
    throw std::system_error(errno, std::generic_category());
  OwnedFd reader{descriptors[0]}, writer{descriptors[1]};
  auto input = take(AsyncFd<>::create(faio::io::io_context::current(), std::move(reader)));
  if (!(::fcntl(input.native_handle(), F_GETFL) & O_NONBLOCK)
      || !(::fcntl(input.native_handle(), F_GETFD) & FD_CLOEXEC))
    co_return false;
  char byte{};
  auto receive = [&](int descriptor) -> faio::expected<std::size_t> {
    const auto count = ::read(descriptor, &byte, 1);
    if (count < 0)
      return std::unexpected{faio::make_error(errno)};
    return static_cast<std::size_t>(count);
  };
  auto empty = input.try_io(faio::io::Interest::readable, receive);
  if (empty || empty.error().value() != EAGAIN)
    co_return false;
  if (::write(writer.get(), "a", 1) != 1)
    co_return false;
  auto old = take(co_await input.readable());
  if (take(old.try_io(receive)) != 1 || byte != 'a')
    co_return false;
  const auto drained = old.try_io(receive);
  if (drained || drained.error().value() != EAGAIN)
    co_return false;
  if (::write(writer.get(), "b", 1) != 1)
    co_return false;
  auto recent = take(co_await input.readable());
  old.clear_ready();
  if (take(recent.try_io(receive)) != 1 || byte != 'b')
    co_return false;
  recent.clear_ready();
  writer = OwnedFd{};
  auto eof = take(co_await input.readable());
  if (take(eof.try_io(receive)) != 0)
    co_return false;
  const int original = input.native_handle();
  auto exported = take(input.into_inner());
  if (exported.get() != original || input.native_handle() != -1)
    co_return false;
  if (::fcntl(exported.get(), F_GETFD) < 0)
    co_return false;
  const int disk = ::open(name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (disk < 0)
    throw std::system_error(errno, std::generic_category());
  auto unsupported = AsyncFd<>::create(faio::io::io_context::current(), OwnedFd{disk});
  co_return !unsupported && unsupported.error().value() == EINVAL;
}
}  // namespace

class IoLifecycleContract : public testing::TestWithParam<faio::runtime::mode> {
 protected:
  auto config() const {
    return faio_test::config_builder().set_mode(GetParam()).set_num_workers(4).build();
  }
};

TEST_P(IoLifecycleContract,
       SocketCommandNumericProtocolAndTypedHelpersPreserveOptionsAndQueueCounts) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(socket_commands_preserve_native_values()));
}

TEST_P(IoLifecycleContract, DeadlineDrainsAndFollowingIoRemainsUsable) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(deadline_and_reuse()));
}

TEST_P(IoLifecycleContract, DirectionConflictNativeExportAndCloseCancel) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(conflict_cancel_close()));
}

TEST_P(IoLifecycleContract, CancelBeforeSubmissionAndWhilePendingResumesExactlyOnce) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(repeated_cancel()));
}

TEST_P(IoLifecycleContract, LiteralCancelFlagsDrainOriginalOperationAndAllowSafeReuse) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(literal_cancel_flags_drain_original_operation()));
}

TEST_P(IoLifecycleContract, AlreadyCancelledImmediateReadAndWriteHaveNoSideEffect) {
  faio_test::runtime_context runtime{config()};
  auto endpoints = runtime.block_on(owned_pair());
  ASSERT_EQ(::send(endpoints.second.as_native_handle(), "r", 1, 0), 1);
  std::latch entered{1}, released{1};
  auto child = runtime.spawn_observed(cancelled_immediate_io(
      endpoints.first.resource(), endpoints.first.resource(), entered, released));
  std::jthread cancellation([&] {
    entered.wait();
    child.request_stop();
    released.count_down();
  });
  runtime.block_on(empty_task());  // current_thread 模式也实际驱动 child 到屏障后的 IO。
  EXPECT_TRUE(child.get());
  EXPECT_TRUE(runtime.block_on(verify_cancelled_io_has_no_side_effect(endpoints)));
}

TEST_P(IoLifecycleContract, WriteHalfShutdownWaitsForFullCompositeDirectionLease) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(half_close_waits_for_composite_reservation()));
}

TEST_P(IoLifecycleContract, ReusedDescriptorDoesNotReceiveOldGenerationEvents) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(descriptor_generation()));
}

TEST_P(IoLifecycleContract, CancelAllStopDrainsReadWithoutDeadlineAndKeepsWrappersSafe) {
  std::optional<std::pair<faio::net::unix::UnixStream, faio::net::unix::UnixStream>> endpoints;
  faio_test::runtime_context runtime{config()};
  endpoints.emplace(runtime.block_on(owned_pair()));
  std::atomic<unsigned> resumed{0};
  auto pending = runtime.spawn_observed(pending_read(endpoints->first.resource(), resumed));
  runtime.block_on(empty_task());
  const auto start = std::chrono::steady_clock::now();
  runtime.stop(faio::io::shutdown_policy::cancel_all);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_EQ(pending.get(), -ECANCELED);
  EXPECT_EQ(resumed.load(), 1u);
  EXPECT_EQ(endpoints->first.as_native_handle(), -1);
  EXPECT_TRUE(endpoints->first.context().stopped());
}

TEST_P(IoLifecycleContract, CancelAllStopCancelsUnobservedLongSleep) {
  faio_test::runtime_context runtime{config()};
  std::atomic<bool> entered{false}, cancelled{false};
  runtime.submit(indefinitely_sleeping_task(entered, cancelled));
  runtime.block_on(empty_task());
  const auto start = std::chrono::steady_clock::now();
  runtime.stop(faio::io::shutdown_policy::cancel_all);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_TRUE(entered.load());
  EXPECT_TRUE(cancelled.load());
}

TEST_P(IoLifecycleContract, AsyncFdGuardGenerationEofExportAndRegularFileRejection) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(async_fd_guards(directory.path() / "disk")));
}

TEST_P(IoLifecycleContract, ResourcesCreatedOutsideRuntimeBindOnFirstCompositeAwait) {
  auto endpoints = take(faio::net::unix::UnixStream::pair(faio::io::io_context{}));
  EXPECT_FALSE(endpoints.first.context());
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(externally_created_composite(endpoints)));
}

INSTANTIATE_TEST_SUITE_P(RuntimeModes,
                         IoLifecycleContract,
                         testing::Values(faio::runtime::mode::current_thread,
                                         faio::runtime::mode::multi_thread));

TEST_P(IoLifecycleContract, CancelledFusedFirstSendHasNoPayloadAndLeavesDirectionUsable) {
  faio_test::runtime_context runtime{config()};
  auto endpoints = runtime.block_on(owned_pair());
  std::latch entered{1}, released{1};
  auto child = runtime.spawn_observed(
      cancelled_fused_first_send(endpoints.first.resource(), entered, released));
  std::jthread cancellation([&] {
    entered.wait();
    child.request_stop();
    released.count_down();
  });
  runtime.block_on(empty_task());
  EXPECT_TRUE(child.get());
  EXPECT_TRUE(runtime.block_on(cancelled_fused_first_send_has_no_payload_and_can_reuse(
      endpoints.first.resource(), endpoints.second.resource())));
}

TEST_P(IoLifecycleContract, FusedFirstSendKeepsReservationAcrossOperationsAndHalfClose) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(fused_reservation_keeps_half_close_order()));
}

TEST_P(IoLifecycleContract, RejectedFusedFirstSendDoesNotLeakDirectionOrPayload) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(fused_reservation_rejected_first_send_releases_direction()));
}

TEST_P(IoLifecycleContract, DeferredWriteAllKeepsResourceAfterUnboundWrapperDestruction) {
  // 尚未归属 runtime 的 wrapper 析构不会请求 domain close；已创建 task
  // 拥有资源。
  auto endpoints = take(faio::net::unix::UnixStream::pair(faio::io::io_context{}));
  const std::weak_ptr<faio::io::detail::resource_state> source = endpoints.first.resource();
  const std::array<char, 4> expected{'k', 'e', 'e', 'p'};
  auto writing = [&] {
    auto original = std::move(endpoints.first);
    return original.write_all(expected);  // 调用时捕获资源，不依赖之后已销毁的 this。
  }();
  EXPECT_FALSE(source.expired());
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(complete_write_after_original_wrapper_destruction(
      std::move(writing), endpoints.second, expected)));
  runtime.stop(faio::io::shutdown_policy::drain);  // 排空 publisher/native close
  // 后才检查无残留。
  EXPECT_TRUE(source.expired());
}
