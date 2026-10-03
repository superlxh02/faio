#include "backend_test_support.hpp"
/**
 * @file test_fs_contract.cpp
 * @brief uring/epoll/kqueue 文件服务与目录 API 的共同契约。
 * @details 验证游标、独占 buffer、短 IO、关闭、错误及显式 context；
 * 文件操作选择原生完成或隔离 provider，测试协程只等待共同的结果协议。
 */
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <gtest/gtest.h>
#include <latch>
#include <limits>
#include <set>
#include <string>
#include <sys/uio.h>
#include <thread>
#include <vector>

namespace {
using faio_test::take;
using namespace std::chrono_literals;

/** @brief read_at/write_at 不改变逻辑游标；clone 共享游标且独立持有 fd。 */
faio::task<bool> file_cursor(const std::filesystem::path &name) {
  auto context = faio::io::io_context::current();
  auto file = take(co_await faio::fs::File::open(
      context, name, faio::fs::OpenOptions{}.read().write().create_new()));
  const std::string payload = "0123456789abcdef";
  take(co_await file.write_all(std::span<const char>{payload}));
  if (take(co_await file.seek(faio::fs::seek_from::current(0))) !=
      payload.size())
    co_return false;
  std::array<char, 4> positional{};
  if (take(co_await file.read_at(positional, 4)) != 4 ||
      std::string_view(positional.data(), 4) != "4567")
    co_return false;
  if (take(co_await file.seek(faio::fs::seek_from::current(0))) !=
      payload.size())
    co_return false;
  take(co_await file.seek(0));
  auto clone = take(co_await file.try_clone());
  std::array<char, 4> first{}, second{};
  if (take(co_await file.read(first)) != 4 ||
      take(co_await clone.read(second)) != 4)
    co_return false;
  if (std::string_view(first.data(), 4) != "0123" ||
      std::string_view(second.data(), 4) != "4567")
    co_return false;
  if (take(co_await file.seek(faio::fs::seek_from::end(-4))) != 12)
    co_return false;
  auto owned = take(co_await file.read(faio::io::io_buffer{4}));
  if (owned.bytes != 4 ||
      std::string_view(owned.buffer.data(), owned.buffer.size()) != "cdef")
    co_return false;
  auto written = take(co_await file.write_at(
      faio::io::io_buffer::copy(std::string_view{"ABCD"}), 4));
  if (written.bytes != 4 || written.buffer.size() != 4)
    co_return false;
  take(co_await file.flush());
  take(co_await file.sync_data());
  take(co_await file.sync_all());
  take(co_await file.set_len(12));
  take(co_await file.set_permissions(
      faio::fs::Permissions{std::filesystem::perms::owner_read |
                            std::filesystem::perms::owner_write}));
  const auto attributes = take(co_await file.metadata());
  if (!attributes.is_file() || attributes.len() != 12 ||
      attributes.permissions().readonly())
    co_return false;
  take(co_await file.close());
  take(co_await file.close());
  const auto closed = co_await file.read(first);
  if (closed || closed.error().value() != EBADF)
    co_return false;
  take(co_await clone.seek(4));
  take(co_await clone.read_exact(second));
  if (std::string_view(second.data(), 4) != "ABCD")
    co_return false;
  take(co_await clone.close());
  co_return true;
}

/** @brief 无效打开选项/已有文件/OOB offset/EOF 不应变成成功或无限循环。 */
faio::task<bool> file_errors(const std::filesystem::path &name) {
  auto invalid = co_await faio::fs::File::open(name, faio::fs::OpenOptions{});
  if (invalid || invalid.error().value() != EINVAL)
    co_return false;
  auto file = take(co_await faio::fs::File::open(
      name, faio::fs::OpenOptions{}.read().write().create_new()));
  auto duplicate = co_await faio::fs::File::open(
      name, faio::fs::OpenOptions{}.write().create_new());
  if (duplicate || duplicate.error().value() != EEXIST)
    co_return false;
  std::array<char, 8> bytes{};
  if (take(co_await file.read(bytes)) != 0)
    co_return false;
  auto exact = co_await file.read_exact(bytes);
  if (exact || exact.error().value() != faio::Error::UnexpectedEOF ||
      exact.error().progress())
    co_return false;
  auto too_far =
      co_await file.read_at(bytes, std::numeric_limits<std::uint64_t>::max());
  if (too_far || too_far.error().value() != EOVERFLOW)
    co_return false;
  auto negative = co_await file.seek(faio::fs::seek_from::current(-1));
  if (negative || negative.error().value() != EINVAL)
    co_return false;
  take(co_await file.close());
  auto appended = take(
      co_await faio::fs::File::open(name, faio::fs::OpenOptions{}.append()));
  const auto positional =
      co_await appended.write_at(std::span<const char>{"x", 1}, 0);
  if (positional || positional.error().value() != EINVAL)
    co_return false;
  take(co_await appended.write_all(std::span<const char>{"xyz", 3}));
  take(co_await appended.close());
  auto read = take(co_await faio::fs::read(name));
  co_return std::string_view(read.data(), read.size()) == "xyz";
}

/** @brief iovec 数组在创建 task 时复制；payload 借用在完成之前仍保持有效。 */
faio::task<bool> file_vectors(const std::filesystem::path &name) {
  auto file = take(co_await faio::fs::File::open(
      name, faio::fs::OpenOptions{}.read().write().create_new()));
  std::array<char, 3> first{'a', 'b', 'c'}, second{'d', 'e', 'f'};
  // lambda 返回时局部 descriptor 数组已经析构；task 只能使用自己的数组副本。
  auto write = [&] {
    std::array<iovec, 3> vectors{{{first.data(), first.size()},
                                  {nullptr, 0},
                                  {second.data(), second.size()}}};
    return file.write_vectored(vectors);
  }();
  auto moved = std::move(file);
  if (take(co_await std::move(write)) != 6)
    co_return false;
  if (take(co_await moved.seek(faio::fs::seek_from::current(0))) != 6)
    co_return false;
  first.fill(0);
  second.fill(0);
  auto read = [&] {
    const std::array<iovec, 2> vectors{
        {{first.data(), first.size()}, {second.data(), second.size()}}};
    return moved.read_vectored_at(vectors, 0);
  }();
  if (take(co_await std::move(read)) != 6 ||
      std::string_view(first.data(), 3) != "abc" ||
      std::string_view(second.data(), 3) != "def")
    co_return false;
  if (take(co_await moved.seek(faio::fs::seek_from::current(0))) != 6)
    co_return false;
  first = {'X', 'Y', 'Z'};
  const std::array<iovec, 1> source{{{first.data(), first.size()}}};
  if (take(co_await moved.write_vectored_at(source, 1)) != 3)
    co_return false;
  if (take(co_await moved.seek(faio::fs::seek_from::current(0))) != 6)
    co_return false;
  take(co_await moved.seek(0));
  const std::array<iovec, 2> destination{
      {{first.data(), first.size()}, {second.data(), second.size()}}};
  if (take(co_await moved.read_vectored(destination)) != 6 ||
      std::string_view(first.data(), 3) != "aXY" ||
      std::string_view(second.data(), 3) != "Zef")
    co_return false;
  if (take(co_await moved.read_vectored(destination)) != 0)
    co_return false;
  if (take(co_await moved.write_vectored({})) != 0)
    co_return false;
  const std::array<iovec, 1> invalid{{{nullptr, 1}}};
  const auto null_payload = co_await moved.read_vectored(invalid);
  if (null_payload || null_payload.error().value() != EFAULT)
    co_return false;
  const auto overflow = co_await moved.read_vectored_at(
      destination, std::numeric_limits<std::uint64_t>::max());
  if (overflow || overflow.error().value() != EOVERFLOW)
    co_return false;
  take(co_await moved.close());
  co_return true;
}

/** @brief 原生 IO awaiter 的普通文件请求在不同后端保留同一外部行为。 */
faio::task<bool> raw_file_fallback(const std::filesystem::path &name) {
  auto opening = faio::io::open(name.string().c_str(),
                                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  faio::io::unix::OwnedFd descriptor{take(co_await std::move(opening))};
  const std::array<char, 6> payload{'r', 'a', 'w', 'I', 'O', '!'};
  const auto null_vectors =
      co_await faio::io::writev(descriptor.get(), nullptr, 1, 0);
  if (null_vectors || null_vectors.error().value() != EFAULT)
    co_return false;
  const auto huge_vectors = co_await faio::io::readv(
      descriptor.get(), nullptr, std::numeric_limits<unsigned>::max(), 0);
  if (huge_vectors || (huge_vectors.error().value() != EINVAL &&
                       huge_vectors.error().value() != EFAULT))
    co_return false;
  const auto null_message =
      co_await faio::io::sendmsg(descriptor.get(), nullptr);
  if (null_message || null_message.error().value() != EFAULT)
    co_return false;
  msghdr invalid_message{};
  invalid_message.msg_iovlen = 1;
  const auto null_message_vectors =
      co_await faio::io::sendmsg(descriptor.get(), &invalid_message);
  if (null_message_vectors || null_message_vectors.error().value() != EFAULT)
    co_return false;
  std::array<char, 1> overflow_buffer{};
  const auto huge_offset =
      co_await faio::io::read(descriptor.get(), overflow_buffer.data(), 1,
                              std::numeric_limits<std::uint64_t>::max() - 1);
  if (huge_offset || (huge_offset.error().value() != EOVERFLOW &&
                      huge_offset.error().value() != EINVAL))
    co_return false;
  if (take(co_await faio::io::write(descriptor.get(), payload.data(),
                                    payload.size(), 0)) != payload.size())
    co_return false;
  std::array<char, 3> first{}, second{};
  auto reading = [&] {
    const std::array<iovec, 2> vectors{
        {{first.data(), first.size()}, {second.data(), second.size()}}};
    return faio::io::readv(descriptor.get(), vectors.data(), vectors.size(), 0);
  }();
  if (take(co_await std::move(reading)) != 6 ||
      std::string_view(first.data(), 3) != "raw" ||
      std::string_view(second.data(), 3) != "IO!")
    co_return false;
  const std::array<iovec, 2> source{
      {{first.data(), first.size()}, {second.data(), second.size()}}};
  if (take(co_await faio::io::writev(descriptor.get(), source.data(),
                                     source.size(), 6)) != 6)
    co_return false;
  take(co_await faio::io::fsync(descriptor.get()));
  const auto unknown_flags = co_await faio::io::writev(
      descriptor.get(), source.data(), source.size(), 0, 0x40000000);
  if (unknown_flags)
    co_return false; // 非零 flags 不得悄悄变成普通 pwritev 并覆盖文件。
  std::array<char, 12> all{};
  if (take(co_await faio::io::read(descriptor.get(), all.data(), all.size(),
                                   0)) != all.size() ||
      std::string_view(all.data(), all.size()) != "rawIO!rawIO!")
    co_return false;
  take(co_await faio::io::close(descriptor.release()));
  co_return true;
}

/** @brief 路径 API、符号链接/硬链接、目录批量枚举覆盖同一临时树。 */
faio::task<bool> directory_operations(const std::filesystem::path &root) {
  auto context = faio::io::io_context::current();
  const auto folder = root / "nested" / "leaf";
  take(co_await faio::fs::create_dir_all(context, folder));
  take(co_await faio::fs::create_dir(root / "single"));
  const auto source = folder / "source";
  const std::string bytes = "异步文件与目录 API\n";
  take(co_await faio::fs::write(context, source, std::span<const char>{bytes}));
  if (take(co_await faio::fs::read_to_string(source)) != bytes)
    co_return false;
  if (!take(co_await faio::fs::metadata(folder)).is_dir())
    co_return false;
  const auto copied = folder / "copy";
  if (take(co_await faio::fs::copy(source, copied)) != bytes.size())
    co_return false;
  take(co_await faio::fs::hard_link(source, folder / "hard"));
  take(co_await faio::fs::symlink(source, folder / "link"));
  if (!take(co_await faio::fs::symlink_metadata(folder / "link")).is_symlink())
    co_return false;
  if (!take(co_await faio::fs::metadata(folder / "link")).is_file())
    co_return false;
  if (take(co_await faio::fs::canonicalize(folder / "link")) !=
      take(co_await faio::fs::canonicalize(source)))
    co_return false;
  take(co_await faio::fs::set_permissions(
      copied, faio::fs::Permissions{std::filesystem::perms::owner_read |
                                    std::filesystem::perms::owner_write}));
  take(co_await faio::fs::rename(copied, folder / "renamed"));
  auto directory = take(co_await faio::fs::read_dir(context, folder));
  std::set<std::string> names;
  for (;;) {
    auto entry = take(co_await directory.next_entry());
    if (!entry)
      break;
    names.insert(entry->file_name().string());
    if (entry->path().parent_path() != folder)
      co_return false;
    (void)take(co_await entry->metadata());
    (void)take(co_await entry->file_type());
  }
  take(co_await directory.close());
  if (names != std::set<std::string>{"source", "hard", "link", "renamed"})
    co_return false;
  take(co_await faio::fs::remove_file(folder / "link"));
  take(co_await faio::fs::remove_file(folder / "hard"));
  take(co_await faio::fs::remove_file(folder / "source"));
  take(co_await faio::fs::remove_file(folder / "renamed"));
  take(co_await faio::fs::remove_dir(root / "single"));
  take(co_await faio::fs::remove_dir_all(root / "nested"));
  const auto missing = co_await faio::fs::metadata(source);
  co_return !missing && missing.error().value() == ENOENT;
}

/** @brief 同一路径、硬链接和符号链接别名不能使 copy 截断原始 inode。 */
faio::task<bool>
copy_aliases_preserve_source(const std::filesystem::path &root) {
  const auto source = root / "copy-source";
  const std::string contents = "same inode must preserve its original contents";
  take(co_await faio::fs::write(source, std::span<const char>{contents}));
  take(co_await faio::fs::hard_link(source, root / "copy-hard"));
  take(co_await faio::fs::symlink(source, root / "copy-symlink"));
  for (const auto &destination :
       {source, root / "copy-hard", root / "copy-symlink"}) {
    const auto result = co_await faio::fs::copy(source, destination);
    if (result)
      co_return false;
    if (take(co_await faio::fs::read_to_string(source)) != contents)
      co_return false;
  }
  co_return true;
}

/** @brief File 组合 task 及目录 task
 * 必须在调用时捕获状态，移动/临时析构后仍有效。 */
faio::task<bool>
delayed_composite_and_directory_tasks(const std::filesystem::path &root) {
  const auto name = root / "composite";
  auto file = take(co_await faio::fs::File::open(
      name, faio::fs::OpenOptions{}.read().write().create_new()));
  std::string expected(256 * 1024, 'q');
  for (std::size_t index = 0; index < expected.size(); ++index)
    expected[index] = static_cast<char>('A' + index % 26);
  auto writing = file.write_all(std::span<const char>{expected});
  auto moved = std::move(file);
  take(co_await std::move(writing));
  take(co_await moved.seek(0));
  std::vector<char> actual(expected.size());
  auto reading = moved.read_exact(actual);
  auto moved_again = std::move(moved);
  take(co_await std::move(reading));
  if (std::string_view(actual.data(), actual.size()) != expected)
    co_return false;
  take(co_await moved_again.close());
  auto type =
      faio::fs::DirEntry{faio::io::io_context::current(), name, DT_UNKNOWN}
          .file_type();
  if (take(co_await std::move(type)) != std::filesystem::file_type::regular)
    co_return false;
  auto creating =
      faio::fs::DirBuilder{}
          .recursive()
          .permissions(faio::fs::Permissions{std::filesystem::perms::owner_all})
          .create(root / "builder" / "leaf");
  take(co_await std::move(creating));
  const auto metadata =
      take(co_await faio::fs::metadata(root / "builder" / "leaf"));
  co_return metadata.is_dir() &&
      (metadata.permissions().bits() & std::filesystem::perms::owner_all) ==
          std::filesystem::perms::owner_all;
}

/** @brief 完整写跨越多个 provider 块时仍独占整个逻辑游标，不能与另一次写交错。
 */
faio::task<bool>
composite_cursor_serialization(const std::filesystem::path &name) {
  auto file = take(co_await faio::fs::File::open(
      name, faio::fs::OpenOptions{}.read().write().create_new()));
  const std::string first(512 * 1024, 'a'), second(512 * 1024, 'b');
  auto one = faio::spawn(file.write_all(std::span<const char>{first}));
  auto two = faio::spawn(file.write_all(std::span<const char>{second}));
  take(co_await one);
  take(co_await two);
  if (take(co_await file.seek(faio::fs::seek_from::current(0))) !=
      first.size() + second.size())
    co_return false;
  take(co_await file.seek(0));
  std::vector<char> actual(first.size() + second.size());
  take(co_await file.read_exact(actual));
  take(co_await file.close());
  const std::string_view view{actual.data(), actual.size()};
  co_return (view.substr(0, first.size()) == first &&
             view.substr(first.size()) == second) ||
      (view.substr(0, second.size()) == second &&
       view.substr(second.size()) == first);
}

/** @brief 深度/批量上限明确拒绝，目录链接仅删除链接自身。 */
faio::task<bool>
recursive_remove_limits_and_symlinks(const std::filesystem::path &root) {
  const auto tree = root / "depth";
  take(co_await faio::fs::create_dir_all(tree / "child"));
  faio::fs::recursive_remove_options options;
  options.max_depth = 1;
  const auto limited = co_await faio::fs::remove_dir_all(tree, options);
  if (limited || limited.error().value() != ELOOP ||
      limited.error().progress() != 0 ||
      !std::filesystem::is_directory(tree / "child"))
    co_return false;
  for (const auto batch : {std::size_t{0}, std::size_t{4097}}) {
    options.batch_size = batch;
    const auto invalid = co_await faio::fs::remove_dir_all(tree, options);
    if (invalid || invalid.error().value() != EINVAL)
      co_return false;
  }
  take(co_await faio::fs::create_dir_all(root / "outside"));
  take(co_await faio::fs::write(root / "outside" / "keep",
                                std::string_view{"preserved"}));
  take(co_await faio::fs::symlink(root / "outside", tree / "linked-directory"));
  options = {};
  options.batch_size = 1;
  if (take(co_await faio::fs::remove_dir_all(tree, options)) != 3)
    co_return false;
  co_return take(co_await faio::fs::read_to_string(root / "outside" /
                                                   "keep")) == "preserved";
}

/** @brief 分批递归删除被取消后，progress
 * 与真实剩余条目一致，未报成功的删除不被隐藏。 */
faio::task<bool>
recursive_remove_cancellation(const std::filesystem::path &tree,
                              std::size_t initial_entries) {
  faio::fs::recursive_remove_options options;
  options.batch_size = 1;
  auto removing = faio::spawn(faio::fs::remove_dir_all(tree, options));
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  std::size_t remaining = initial_entries;
  do {
    co_await faio::time::sleep(1ms);
    remaining = static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator{tree},
                      std::filesystem::directory_iterator{}));
  } while (remaining == initial_entries &&
           std::chrono::steady_clock::now() < deadline);
  removing.request_stop();
  const auto result = co_await removing;
  if (result)
    throw std::runtime_error(
        "cancelled recursive remove returned success; removed=" +
        std::to_string(*result));
  if (result.error().value() != ECANCELED)
    throw std::runtime_error("recursive remove cancellation error=" +
                             std::to_string(result.error().value()));
  remaining = static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator{tree},
                    std::filesystem::directory_iterator{}));
  if (result.error().progress() > 0 && remaining > 0 &&
      result.error().progress() == initial_entries - remaining)
    co_return true;
  throw std::runtime_error(
      "recursive remove actual_removed=" +
      std::to_string(initial_entries - remaining) +
      " reported_progress=" + std::to_string(result.error().progress()) +
      " remaining=" + std::to_string(remaining));
}

faio::task<int> blocking_execution_with_deadline(faio::io::io_context context,
                                                 std::atomic<bool> &entered,
                                                 std::latch &release) {
  auto result = co_await faio::execution::execute(context, [&] {
                  entered.store(true, std::memory_order_release);
                  release.wait();
                  return 0;
                }).set_timeout(200ms);
  co_return result ? *result : -result.error().value();
}
/** @brief 首先获得的 stop
 * 原因不能被随后到期的deadline覆盖；借用要等job真正排空。 */
faio::task<bool> execution_preserves_first_cancel_reason() {
  std::atomic<bool> entered{false};
  std::latch release{1};
  auto pending = faio::spawn(blocking_execution_with_deadline(
      faio::io::io_context::current(), entered, release));
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!entered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline)
    co_await faio::time::sleep(1ms);
  if (!entered.load(std::memory_order_acquire)) {
    release.count_down();
    pending.request_stop();
    (void)co_await pending;
    co_return false;
  }
  pending.request_stop();
  co_await faio::time::sleep(250ms);
  release.count_down();
  co_return co_await pending == -ECANCELED;
}

/**
 * @brief 原生文件、属性和路径操作不依赖应用层文件/DNS/清理辅助线程。
 * @details 同时占满三条辅助服务，再验证真实 SQE 提交及 CQE 完成；
 * 不支持原生操作的 epoll/kqueue 仍验证同一行为，但原生计数不得增长。
 */
faio::task<bool> file_backend_path_evidence(const std::filesystem::path &name) {
  auto context = faio::io::io_context::current();
  const bool native = context.domain()->capabilities().native_filesystem;
  const std::array path_opcodes{faio::io::detail::operation_kind::statx,
                                faio::io::detail::operation_kind::mkdirat,
                                faio::io::detail::operation_kind::unlinkat,
                                faio::io::detail::operation_kind::renameat,
                                faio::io::detail::operation_kind::linkat,
                                faio::io::detail::operation_kind::symlinkat,
                                faio::io::detail::operation_kind::ftruncate};
  const bool native_paths =
      native &&
      std::all_of(path_opcodes.begin(), path_opcodes.end(), [&](auto kind) {
        return context.domain()->supports_native(kind);
      });
  if (native && (context.blocking().started_threads() != 0 ||
                 context.resolver().started_threads() != 0 ||
                 context.cleanup().started_threads() != 0))
    co_return false; // 原生 runtime 不应预先启动无任务的辅助服务线程。
  const auto before = context.statistics();
  struct lane_barrier {
    std::atomic<unsigned> entered{};
    std::atomic<bool> released{}, watchdog_released{};
    std::latch release{1};
    void unblock(bool watchdog = false) noexcept {
      if (!released.exchange(true, std::memory_order_acq_rel)) {
        watchdog_released.store(watchdog, std::memory_order_release);
        release.count_down();
      }
    }
  };
  auto barrier = std::make_shared<lane_barrier>();
  struct unblock_guard {
    std::shared_ptr<lane_barrier> barrier;
    ~unblock_guard() { barrier->unblock(); }
  } unblock{barrier};
  std::jthread watchdog;
  if (native) {
    // 每条服务只配置一个线程；三个 job 共用闸门，原生路径不能借道它们。
    for (auto service :
         {context.blocking(), context.resolver(), context.cleanup()}) {
      take(service.try_submit([barrier] {
        barrier->entered.fetch_add(1, std::memory_order_release);
        barrier->release.wait();
      }));
    }
    const auto entering_deadline = std::chrono::steady_clock::now() + 2s;
    while (barrier->entered.load(std::memory_order_acquire) != 3 &&
           std::chrono::steady_clock::now() < entering_deadline)
      co_await faio::time::sleep(1ms);
    if (barrier->entered.load(std::memory_order_acquire) != 3)
      co_return false;
    watchdog = std::jthread([barrier](std::stop_token stop) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!stop.stop_requested() &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
      if (!stop.stop_requested())
        barrier->unblock(true);
    }); // 失败时释放所有服务；触发即判失败，不把误路由变成永久死锁。
  }
  auto file = take(co_await faio::fs::File::open(
      context, name, faio::fs::OpenOptions{}.read().write().create_new()));
  const std::array<char, 8> payload{'n', 'a', 't', 'i', 'v', 'e', 'I', 'O'};
  if (take(co_await file.write_at(payload, 0)) != payload.size())
    co_return false;
  std::array<char, 4> first{}, second{};
  const std::array<iovec, 2> vectors{
      {{first.data(), first.size()}, {second.data(), second.size()}}};
  if (take(co_await file.read_vectored_at(vectors, 0)) != payload.size())
    co_return false;
  if (std::string_view(first.data(), first.size()) != "nati" ||
      std::string_view(second.data(), second.size()) != "veIO")
    co_return false;
  auto owned = take(co_await file.read_at(faio::io::io_buffer{8}, 0));
  if (owned.bytes != payload.size() ||
      std::string_view(owned.buffer.data(), owned.buffer.size()) != "nativeIO")
    co_return false;
  if (take(co_await file.write_vectored_at(vectors, payload.size())) !=
      payload.size())
    co_return false;
  take(co_await file.sync_data());
  take(co_await file.sync_all());
  // 旧内核没有对应 opcode 时只验证公开行为；已支持的现代内核必须继续
  // 在三个服务仍被占满时完成全部路径操作，不能掩盖支持能力与实际路由不符。
  if (native && !native_paths)
    barrier->unblock();
  take(co_await file.set_len(16)); // FTRUNCATE 必须保持同一个 fd 租约。
  const auto attributes = take(co_await file.metadata()); // fd STATX。
  const auto path_attributes = take(co_await faio::fs::metadata(context, name));
  const auto folder = name.parent_path() / "native-directory";
  take(co_await faio::fs::create_dir(context, folder));
  if (!take(co_await faio::fs::metadata(context, folder)).is_dir())
    co_return false;
  auto directory = take(co_await faio::fs::read_dir(context, folder));
  take(co_await directory.close()); // 只有枚举缺 opcode；OPEN/CLOSE 仍须原生。
  const auto renamed = folder / "renamed";
  const auto hard = folder / "hard";
  const auto symbolic = folder / "symbolic";
  take(co_await faio::fs::rename(context, name, renamed));
  take(co_await faio::fs::hard_link(context, renamed, hard));
  take(co_await faio::fs::symlink(context, renamed, symbolic));
  const auto link_attributes =
      take(co_await faio::fs::symlink_metadata(context, symbolic));
  const auto followed = take(co_await faio::fs::metadata(context, symbolic));
  if (!link_attributes.is_symlink() || !followed.is_file() ||
      !attributes.same_file(followed))
    co_return false;
  take(co_await faio::fs::remove_file(context, hard));
  take(co_await faio::fs::remove_file(context, symbolic));
  const auto nested = folder / "recursive" / "leaf";
  take(co_await faio::fs::create_dir_all(context, nested));
  take(co_await faio::fs::remove_dir(context, nested));
  take(co_await faio::fs::remove_dir(context, nested.parent_path()));
  take(co_await file.close());
  take(co_await faio::fs::remove_file(context, renamed));
  take(co_await faio::fs::remove_dir(context, folder));
  watchdog.request_stop();
  const auto after = context.statistics();
  if (attributes.len() != 16 || path_attributes.len() != 16 ||
      barrier->watchdog_released.load(std::memory_order_acquire))
    co_return false;
  if (native) {
    // 文件 IO、STATX/FTRUNCATE 和每个目录/路径动作均需真实业务 CQE。
    // create_dir_all 对已存在父目录的校验会额外提交 MKDIRAT/STATX。
    const auto minimum = native_paths ? 28 : 8;
    co_return after.native_completed >=
        before.native_completed + minimum &&after.native_submitted >=
        before.native_submitted + minimum &&after.native_flushed >=
        before.native_flushed + minimum;
  }
  co_return after.native_submitted ==
      before.native_submitted &&after.native_completed ==
      before.native_completed &&after.native_flushed == before.native_flushed;
}
} // namespace

class FilesystemContract : public testing::TestWithParam<faio::runtime::mode> {
};
TEST_P(FilesystemContract, CursorPositionalIoOwnedBuffersCloneAndClose) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(file_cursor(directory.path() / "file")));
}
TEST_P(FilesystemContract, InvalidOptionsOffsetsEofAndAppend) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(file_errors(directory.path() / "file")));
}
TEST_P(FilesystemContract,
       VectoredCursorOffsetsDescriptorArrayLifetimeAndMove) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(file_vectors(directory.path() / "vectors")));
}
TEST_P(FilesystemContract,
       RawOpenReadWriteVectorsFsyncClosePreserveBackendContract) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(raw_file_fallback(directory.path() / "raw")));
}
TEST_P(FilesystemContract, EveryPathAndDirectoryOperation) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(directory_operations(directory.path())));
}
TEST_P(FilesystemContract, CopyToSamePathHardLinkAndSymlinkPreservesSource) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(copy_aliases_preserve_source(directory.path())));
}
TEST_P(FilesystemContract,
       CompositeTasksAndTemporaryDirectoryObjectsCaptureState) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(
      delayed_composite_and_directory_tasks(directory.path())));
}
TEST_P(FilesystemContract, ConcurrentCompleteWritesKeepCursorBlocksTogether) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(
      composite_cursor_serialization(directory.path() / "cursor-serialized")));
}
TEST_P(FilesystemContract,
       RecursiveRemoveDepthBatchLimitsAndDirectorySymlinkSafety) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(
      runtime.block_on(recursive_remove_limits_and_symlinks(directory.path())));
}
TEST_P(FilesystemContract,
       RecursiveRemoveCancellationReportsActualRemovedEntries) {
  faio_test::temporary_directory directory;
  const auto tree = directory.path() / "cancellable";
  std::filesystem::create_directory(tree);
  constexpr std::size_t entries = 2048;
  // 同步fixture在runtime启动前创建，测量/取消阶段只由异步接口处理。
  for (std::size_t index = 0; index < entries; ++index) {
    const int descriptor = ::open((tree / std::to_string(index)).c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL, 0600);
    ASSERT_GE(descriptor, 0);
    ASSERT_EQ(::close(descriptor), 0);
  }
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(recursive_remove_cancellation(tree, entries)));
}
TEST_P(FilesystemContract,
       BlockingExecuteKeepsEarlierStopReasonAfterDeadlineAndDrain) {
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(execution_preserves_first_cancel_reason()));
}
TEST_P(FilesystemContract,
       NativeFileMetadataAndPathOpcodesProgressWithAllAuxiliaryLanesBusy) {
  faio_test::temporary_directory directory;
  faio_test::runtime_context runtime{faio_test::config_builder()
                                         .set_mode(GetParam())
                                         .set_num_workers(4)
                                         .set_filesystem_threads(1)
                                         .set_resolver_threads(1)
                                         .build()};
  EXPECT_TRUE(runtime.block_on(
      file_backend_path_evidence(directory.path() / "native-path")));
}
INSTANTIATE_TEST_SUITE_P(RuntimeModes, FilesystemContract,
                         testing::Values(faio::runtime::mode::current_thread,
                                         faio::runtime::mode::multi_thread));
