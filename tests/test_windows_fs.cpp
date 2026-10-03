/** @file test_windows_fs.cpp @brief Windows 文件/目录全部公开 API 的集成契约。 */
#include "faio/faio.hpp"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <latch>
#include <set>
#include <source_location>
#include <string>
#include <thread>
#include <winioctl.h>

namespace {
template <class T>
T take(faio::expected<T> result, std::source_location source = std::source_location::current()) {
  if (!result)
    throw std::runtime_error("IO error=" + std::to_string(result.error().value()) + ", domain="
                             + std::to_string(static_cast<int>(result.error().domain()))
                             + ", line=" + std::to_string(source.line()));
  return std::move(*result);
}

void take(faio::expected<void> result,
          std::source_location source = std::source_location::current()) {
  if (!result)
    throw std::runtime_error("IO error=" + std::to_string(result.error().value())
                             + ", line=" + std::to_string(source.line()));
}

/** @brief 独立临时目录；runtime/文件状态在外层目录销毁前完成清理。 */
struct temporary_directory {
  std::filesystem::path path;

  temporary_directory() {
    static std::atomic<unsigned long> sequence{};
    path = std::filesystem::temp_directory_path()
           / (L"faio-windows-fs-" + std::to_wstring(::GetCurrentProcessId()) + L"-"
              + std::to_wstring(++sequence));
    std::filesystem::create_directory(path);
  }

  ~temporary_directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

using runtime_context = faio::runtime::detail::runtime_context;

class WindowsFilesystem : public testing::TestWithParam<faio::runtime::mode> {};

auto config(faio::runtime::mode mode) {
  return faio::config_builder{}.set_mode(mode).set_num_workers(4).build();
}

/** @brief 游标、位置 IO、克隆、拥有 buffer、持久化、截断和幂等关闭。 */
faio::task<bool> file_contract(std::filesystem::path name) {
  auto context = faio::io::io_context::current();
  auto file = take(co_await faio::fs::File::open(
      context, name, faio::fs::OpenOptions{}.read().write().create_new()));
  static_assert(sizeof(decltype(file.native_handle())) == sizeof(void*));
  const std::string text = "0123456789abcdef";
  take(co_await file.write_all(std::span<const char>{text}));
  std::array<char, 4> bytes{};
  if (take(co_await file.read_at(bytes, 4)) != 4 || std::string_view(bytes.data(), 4) != "4567")
    co_return false;
  if (take(co_await file.seek(faio::fs::seek_from::current(0))) != 16)
    co_return false;
  take(co_await file.seek(0));
  auto clone = take(co_await file.try_clone());
  take(co_await file.read_exact(bytes));
  if (std::string_view(bytes.data(), 4) != "0123")
    co_return false;
  take(co_await clone.read_exact(bytes));
  if (std::string_view(bytes.data(), 4) != "4567")
    co_return false;
  if (take(co_await file.seek(faio::fs::seek_from::end(-4))) != 12)
    co_return false;
  auto owned = take(co_await file.read(faio::io::io_buffer{4}));
  if (owned.bytes != 4 || std::string_view(owned.buffer.data(), 4) != "cdef")
    co_return false;
  auto written =
      take(co_await file.write_at(faio::io::io_buffer::copy(std::string_view{"ABCD"}), 4));
  if (written.bytes != 4)
    co_return false;
  take(co_await file.flush());
  take(co_await file.sync_data());
  take(co_await file.sync_all());
  take(co_await file.set_len(12));
  faio::fs::Permissions permissions{std::filesystem::perms::all};
  permissions.set_readonly(true);
  take(co_await file.set_permissions(permissions));
  if (!take(co_await file.permissions()).readonly())
    co_return false;
  permissions.set_readonly(false);
  take(co_await file.set_permissions(permissions));
  const auto metadata = take(co_await file.metadata());
  if (!metadata.is_file() || metadata.len() != 12 || metadata.permissions().readonly())
    co_return false;
  const auto native = file.native_handle();
  if (!faio::fs::detail::valid_handle(native) || !file.context())
    co_return false;
  take(co_await file.close());
  take(co_await file.close());
  auto invalid = co_await file.read(bytes);
  if (invalid || invalid.error().value() != EBADF)
    co_return false;
  take(co_await clone.seek(4));
  take(co_await clone.read_exact(bytes));
  if (std::string_view(bytes.data(), 4) != "ABCD")
    co_return false;
  take(co_await clone.close());
  co_return true;
}

TEST_P(WindowsFilesystem, CompleteFileApiCursorCloneMetadataPermissionsAndClose) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(file_contract(directory.path / L"文件数据.bin")));
}

/** @brief 数组在 task 构造时复制，File 移动后 task 仍保持状态与 payload 租约。 */
faio::task<bool> vector_contract(std::filesystem::path name) {
  auto file = take(
      co_await faio::fs::File::open(name, faio::fs::OpenOptions{}.read().write().create_new()));
  std::array<char, 3> first{'a', 'b', 'c'}, second{'d', 'e', 'f'};
  auto write = [&] {
    const std::array<iovec, 3> vectors{{{first.data(), 3}, {nullptr, 0}, {second.data(), 3}}};
    return file.write_vectored(vectors);
  }();
  auto moved = std::move(file);
  if (take(co_await std::move(write)) != 6)
    co_return false;
  first.fill(0);
  second.fill(0);
  const std::array<iovec, 2> vectors{{{first.data(), 3}, {second.data(), 3}}};
  if (take(co_await moved.read_vectored_at(vectors, 0)) != 6
      || std::string_view(first.data(), 3) != "abc" || std::string_view(second.data(), 3) != "def")
    co_return false;
  first = {'X', 'Y', 'Z'};
  if (take(co_await moved.write_vectored_at(std::span<const iovec>{vectors}.first(1), 1)) != 3)
    co_return false;
  take(co_await moved.seek(0));
  if (take(co_await moved.read_vectored(vectors)) != 6 || std::string_view(first.data(), 3) != "aXY"
      || std::string_view(second.data(), 3) != "Zef")
    co_return false;
  if (take(co_await moved.read_vectored(vectors)) != 0
      || take(co_await moved.write_vectored({})) != 0)
    co_return false;
  const std::array<iovec, 1> invalid{{{nullptr, 1}}};
  const auto fault = co_await moved.read_vectored(invalid);
  if (fault || fault.error().value() != EFAULT)
    co_return false;
  take(co_await moved.close());
  co_return true;
}

TEST_P(WindowsFilesystem, VectoredBorrowedPayloadDescriptorsOwnLifetimeAndMove) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(vector_contract(directory.path / "vectors")));
}

faio::task<bool> errors_and_append(std::filesystem::path name) {
  auto invalid = co_await faio::fs::File::open(name, faio::fs::OpenOptions{});
  if (invalid || invalid.error().value() != EINVAL)
    co_return false;
  auto file = take(
      co_await faio::fs::File::open(name, faio::fs::OpenOptions{}.read().write().create_new()));
  auto exists = co_await faio::fs::File::open(name, faio::fs::OpenOptions{}.write().create_new());
  if (exists || exists.error().domain() != faio::error_domain::win32
      || exists.error().value() != ERROR_FILE_EXISTS)
    co_return false;
  std::array<char, 8> bytes{};
  if (take(co_await file.read(bytes)) != 0)
    co_return false;
  const auto eof = co_await file.read_exact(bytes);
  if (eof || eof.error().value() != faio::Error::UnexpectedEOF)
    co_return false;
  const auto overflow = co_await file.read_at(bytes, UINT64_MAX);
  if (overflow || overflow.error().value() != EOVERFLOW)
    co_return false;
  const auto negative = co_await file.seek(faio::fs::seek_from::current(-1));
  if (negative || negative.error().value() != EINVAL)
    co_return false;
  take(co_await file.close());
  auto append = take(co_await faio::fs::File::open(name, faio::fs::OpenOptions{}.append()));
  const auto positional = co_await append.write_at(std::span<const char>{"x", 1}, 0);
  if (positional || positional.error().value() != EINVAL)
    co_return false;
  take(co_await append.write_all(std::span<const char>{"abc", 3}));
  take(co_await append.write(faio::io::io_buffer::copy(std::string_view{"def"})));
  take(co_await append.close());
  co_return take(co_await faio::fs::read_to_string(name)) == "abcdef";
}

TEST_P(WindowsFilesystem, OpenOptionsNativeErrorDomainEofOffsetsAndAppend) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(errors_and_append(directory.path / "append")));
}

/** @brief 所有路径 API、别名自复制保护、中文及超过 MAX_PATH 的路径。 */
faio::task<bool> path_contract(std::filesystem::path root) {
  auto context = faio::io::io_context::current();
  auto long_path = root;
  for (int level = 0; level < 8; ++level)
    long_path /= L"目录_0123456789012345678901234567890123456789";
  take(co_await faio::fs::DirBuilder{}
           .recursive()
           .permissions(faio::fs::Permissions{std::filesystem::perms::all})
           .create(context, long_path));
  const auto source = long_path / L"中文文件.txt";
  take(co_await faio::fs::write(
      context, source, faio::io::io_buffer::copy(std::string_view{"payload"})));
  const auto attributes = take(co_await faio::fs::metadata(context, source));
  if (!attributes.is_file() || attributes.len() != 7)
    co_return false;
  if (!take(co_await faio::fs::symlink_metadata(source)).is_file())
    co_return false;
  const auto canonical = take(co_await faio::fs::canonicalize(context, source));
  if (!take(co_await faio::fs::metadata(canonical)).same_file(attributes))
    co_return false;
  const auto alias = root / "alias";
  take(co_await faio::fs::hard_link(context, source, alias));
  const auto self = co_await faio::fs::copy(context, source, alias);
  if (self || self.error().value() != EINVAL
      || take(co_await faio::fs::read_to_string(source)) != "payload")
    co_return false;
  const auto copied = root / "copied";
  if (take(co_await faio::fs::copy(context, source, copied)) != 7)
    co_return false;
  take(co_await faio::fs::set_permissions(
      context, copied, faio::fs::Permissions{std::filesystem::perms::all}));
  const auto renamed = root / "renamed";
  take(co_await faio::fs::rename(context, copied, renamed));
  const auto limited = co_await faio::fs::read(context, renamed, 2);
  if (limited || limited.error().value() != EFBIG || limited.error().progress() != 2)
    co_return false;
  take(co_await faio::fs::remove_file(context, alias));
  take(co_await faio::fs::remove_file(renamed));
  const auto empty = root / "empty";
  take(co_await faio::fs::create_dir(empty));
  take(co_await faio::fs::remove_dir(empty));
  const auto removed = take(
      co_await faio::fs::remove_dir_all(root / L"目录_0123456789012345678901234567890123456789"));
  co_return removed == 9;
}

TEST_P(WindowsFilesystem, AllPathApisUnicodeLongPathsAliasesAndCopyLimit) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(path_contract(directory.path)));
}

/** @brief 200 项越过单批缓存；临时 ReadDir 构造的任务保持完整拥有状态。 */
faio::task<bool> directory_contract(std::filesystem::path root) {
  for (int i = 0; i < 200; ++i)
    take(co_await faio::fs::write(root / ("entry-" + std::to_string(i)), std::string_view{"x"}));
  auto directory = take(co_await faio::fs::read_dir(root));
  std::set<std::filesystem::path> names;
  for (;;) {
    auto entry = take(co_await directory.next_entry());
    if (!entry)
      break;
    if (take(co_await entry->file_type()) != std::filesystem::file_type::regular
        || take(co_await entry->metadata()).len() != 1)
      co_return false;
    if (entry->path() != root / entry->file_name())
      co_return false;
    names.insert(entry->file_name());
  }
  take(co_await directory.close());
  take(co_await directory.close());
  const auto closed = co_await directory.next_entry();
  if (closed || closed.error().value() != EBADF || names.size() != 200)
    co_return false;
  auto delayed = take(co_await faio::fs::read_dir(root)).next_entry();
  if (!take(co_await std::move(delayed)))
    co_return false;
  const auto count = take(co_await faio::fs::remove_dir_all(
      root, {.max_depth = 8, .max_state_bytes = 128 * 1024, .batch_size = 3}));
  co_return count == 201;
}

TEST_P(WindowsFilesystem, BoundedDirectoryBatchesEntryApisDelayedTaskAndRecursiveRemove) {
  temporary_directory directory;
  const auto root = directory.path / "tree";
  std::filesystem::create_directory(root);
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(directory_contract(root)));
}

/** @brief 符号链接创建/元数据与目录链接删除；缺 OS 权限明确返回原生错误。 */
faio::task<bool> symlink_contract(std::filesystem::path root) {
  const auto target = root / "target";
  take(co_await faio::fs::create_dir(target));
  take(co_await faio::fs::write(target / "keep", std::string_view{"safe"}));
  const auto tree = root / "tree";
  take(co_await faio::fs::create_dir(tree));
  auto link = co_await faio::fs::symlink(target, tree / "link");
  if (!link) {
    // Windows 未开启 Developer Mode 且没有 SeCreateSymbolicLinkPrivilege 是明确 OS 限制。
    co_return link.error().domain() == faio::error_domain::win32&& link.error().value()
        == ERROR_PRIVILEGE_NOT_HELD;
  }
  if (!take(co_await faio::fs::symlink_metadata(tree / "link")).is_symlink())
    co_return false;
  if (!take(co_await faio::fs::metadata(tree / "link")).is_dir())
    co_return false;
  if (take(co_await faio::fs::remove_dir_all(tree)) != 2)
    co_return false;
  co_return take(co_await faio::fs::read_to_string(target / "keep")) == "safe";
}

TEST_P(WindowsFilesystem, DirectorySymlinkDoesNotTraverseItsTarget) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(symlink_contract(directory.path)));
}

/** @brief 创建免提权 NTFS junction，让安全测试不依赖符号链接创建权限。 */
void make_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
  std::filesystem::create_directory(link);
  faio::io::windows::owned_file_handle handle{
      ::CreateFileW(link.c_str(),
                    GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr)};
  if (!handle)
    throw std::system_error(::GetLastError(), std::system_category());
  const std::wstring substitute = L"\\??\\" + std::filesystem::absolute(target).native();
  const std::wstring print = std::filesystem::absolute(target).native();
  const USHORT substitute_bytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
  const USHORT print_bytes = static_cast<USHORT>(print.size() * sizeof(wchar_t));
  const USHORT print_offset = static_cast<USHORT>(substitute_bytes + sizeof(wchar_t));
  const USHORT data_bytes = static_cast<USHORT>(8 + print_offset + print_bytes + sizeof(wchar_t));
  std::vector<std::byte> buffer(8 + data_bytes);
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(buffer.data(), &tag, 4);
  std::memcpy(buffer.data() + 4, &data_bytes, 2);
  std::memcpy(buffer.data() + 10, &substitute_bytes, 2);
  std::memcpy(buffer.data() + 12, &print_offset, 2);
  std::memcpy(buffer.data() + 14, &print_bytes, 2);
  std::memcpy(buffer.data() + 16, substitute.data(), substitute_bytes);
  std::memcpy(buffer.data() + 16 + print_offset, print.data(), print_bytes);
  DWORD returned{};
  if (!::DeviceIoControl(handle.get(),
                         FSCTL_SET_REPARSE_POINT,
                         buffer.data(),
                         static_cast<DWORD>(buffer.size()),
                         nullptr,
                         0,
                         &returned,
                         nullptr))
    throw std::system_error(::GetLastError(), std::system_category());
}

faio::task<bool> junction_contract(std::filesystem::path root) {
  const auto outside = root / "outside", tree = root / "tree";
  take(co_await faio::fs::create_dir(outside));
  take(co_await faio::fs::write(outside / "keep", std::string_view{"preserved"}));
  take(co_await faio::fs::create_dir_all(tree / "child"));
  faio::fs::recursive_remove_options limits;
  limits.max_depth = 1;
  const auto depth = co_await faio::fs::remove_dir_all(tree, limits);
  if (depth || depth.error().value() != ELOOP || depth.error().progress() != 0)
    co_return false;
  limits.batch_size = 0;
  const auto batch = co_await faio::fs::remove_dir_all(tree, limits);
  if (batch || batch.error().value() != EINVAL)
    co_return false;
  make_junction(tree / "junction", outside);
  if (!take(co_await faio::fs::symlink_metadata(tree / "junction")).is_symlink())
    co_return false;
  if (take(co_await faio::fs::remove_dir_all(
          tree, {.max_depth = 8, .max_state_bytes = 128 * 1024, .batch_size = 1}))
      != 3)
    co_return false;
  make_junction(root / "root-junction", outside);
  if (take(co_await faio::fs::remove_dir_all(root / "root-junction")) != 1)
    co_return false;
  make_junction(root / "file-api-junction", outside);
  take(co_await faio::fs::remove_file(root
                                      / "file-api-junction"));  // 文件删除 API 只移除目录链接自身。
  co_return take(co_await faio::fs::read_to_string(outside / "keep")) == "preserved";
}

TEST_P(WindowsFilesystem, RecursiveDepthBatchBoundsAndJunctionNeverTraverseOutsideTree) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(junction_contract(directory.path)));
}

/** @brief 两个 clone 完整写必须按整块分组，之后并发 close 共用同一真实完成事件。 */
faio::task<bool> concurrent_cursor_and_close(std::filesystem::path name) {
  auto file = take(
      co_await faio::fs::File::open(name, faio::fs::OpenOptions{}.read().write().create_new()));
  auto clone = take(co_await file.try_clone());
  const std::string first(512 * 1024, 'a'), second(512 * 1024, 'b');
  auto one = faio::spawn(file.write_all(std::span<const char>{first}));
  auto two = faio::spawn(clone.write_all(std::span<const char>{second}));
  take(co_await one);
  take(co_await two);
  take(co_await file.seek(0));
  std::vector<char> data(first.size() + second.size());
  take(co_await file.read_exact(data));
  const std::string_view view{data.data(), data.size()};
  const bool grouped =
      (view.substr(0, first.size()) == first && view.substr(first.size()) == second)
      || (view.substr(0, second.size()) == second && view.substr(second.size()) == first);
  auto close_one = faio::spawn(file.close()), close_two = faio::spawn(file.close());
  take(co_await close_one);
  take(co_await close_two);
  take(co_await clone.close());
  co_return grouped;
}

TEST_P(WindowsFilesystem, ConcurrentCloneWritesKeepCompleteTransfersTogetherAndCloseOnce) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(concurrent_cursor_and_close(directory.path / "concurrent")));
}

/** @brief 删除取消要等已开始的原生操作排空，报告条数与真实文件系统副作用一致。 */
faio::task<bool> cancelled_remove(std::filesystem::path root, std::size_t initial) {
  using namespace std::chrono_literals;
  auto removing = faio::spawn(faio::fs::remove_dir_all(
      root, {.max_depth = 8, .max_state_bytes = 128 * 1024, .batch_size = 1}));
  std::size_t remaining = initial;
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (remaining == initial && std::chrono::steady_clock::now() < deadline) {
    co_await faio::time::sleep(1ms);
    remaining = static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator{root},
                                                       std::filesystem::directory_iterator{}));
  }
  removing.request_stop();
  const auto result = co_await removing;
  if (result || result.error().value() != ECANCELED)
    co_return false;
  remaining = static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator{root},
                                                     std::filesystem::directory_iterator{}));
  co_return result.error().progress() > 0 && remaining > 0
      && result.error().progress() == initial - remaining;
}

TEST_P(WindowsFilesystem, RecursiveCancellationPreservesExactlyTheCompletedDeleteCount) {
  temporary_directory directory;
  const auto root = directory.path / "cancel";
  std::filesystem::create_directory(root);
  constexpr std::size_t count = 1200;
  for (std::size_t i = 0; i < count; ++i) {
    const auto name = root / std::to_wstring(i);
    faio::io::windows::owned_file_handle file{
        ::CreateFileW(name.c_str(),
                      GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr,
                      CREATE_NEW,
                      FILE_ATTRIBUTE_NORMAL,
                      nullptr)};
    ASSERT_TRUE(file);
  }
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(cancelled_remove(root, count)));
}

/** @brief 所有文件辅助线程被阻塞时，普通磁盘读写仍只靠原生 IOCP 完成。 */
faio::task<bool> native_reads_without_file_lane(std::filesystem::path name) {
  auto context = faio::io::io_context::current();
  auto file = take(co_await faio::fs::File::open(
      context, name, faio::fs::OpenOptions{}.read().write().create_new()));
  std::latch started{4}, release{1}, finished{4};

  struct unblock {
    std::latch &gate, &finished;

    ~unblock() {
      gate.count_down();
      finished.wait();
    }  // 借用 latch 在所有 job 返回前保持存活。
  } guard{release, finished};

  for (int i = 0; i < 4; ++i)
    take(context.blocking().try_submit([&] {
      started.count_down();
      release.wait();
      finished.count_down();
    }));
  started.wait();
  const auto before = context.statistics().native_submitted;
  std::string input(512 * 1024, 'n'), output(input.size(), '\0');
  take(co_await file.write_all(std::span<const char>{input}));
  take(co_await file.seek(0));
  take(co_await file.read_exact(std::span<char>{output}));
  co_return input == output&& context.statistics().native_submitted >= before + 16;
}

TEST_P(WindowsFilesystem, DiskIoUsesNativeCompletionWhenFileServiceIsSaturated) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(native_reads_without_file_lane(directory.path / "native")));
}

/** @brief 外部可调用的 OS 游标检查，不能只验证库内部记录的逻辑位置。 */
std::uint64_t os_file_position(HANDLE handle) {
  LARGE_INTEGER zero{}, current{};
  if (!::SetFilePointerEx(handle, zero, &current, FILE_CURRENT))
    throw std::runtime_error("SetFilePointerEx query failed");
  return static_cast<std::uint64_t>(current.QuadPart);
}

void set_os_file_position(HANDLE handle, std::uint64_t position) {
  LARGE_INTEGER current{};
  current.QuadPart = static_cast<LONGLONG>(position);
  if (!::SetFilePointerEx(handle, current, nullptr, FILE_BEGIN))
    throw std::runtime_error("SetFilePointerEx update failed");
}

faio::task<bool> raw_default_cursor_contract(std::filesystem::path name) {
  const auto utf8 = name.u8string();
  faio::io::windows::owned_file_handle handle{take(co_await faio::io::open(
      reinterpret_cast<const char*>(utf8.c_str()),
      _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY | _O_RANDOM | _O_NOINHERIT))};
  if (take(co_await faio::io::write(handle.get(), "abc", 3)) != 3
      || os_file_position(handle.get()) != 3)
    co_return false;
  set_os_file_position(handle.get(), 1);  // 默认写覆盖当前位置，不能被错误地转成 EOF append。
  if (take(co_await faio::io::write(handle.get(), "XY", 2)) != 2
      || os_file_position(handle.get()) != 3)
    co_return false;
  if (take(co_await faio::io::write(handle.get(), "q", 1, 0)) != 1
      || os_file_position(handle.get()) != 3)
    co_return false;  // 显式位置请求仍经过 IOCP，不能改变隐式 OS 游标。
  set_os_file_position(handle.get(), 0);
  std::array<char, 3> first{};
  if (take(co_await faio::io::read(handle.get(), first.data(), first.size())) != 3
      || std::string_view(first.data(), 3) != "qXY" || os_file_position(handle.get()) != 3)
    co_return false;
  std::array<iovec, 2> output{{{const_cast<char*>("1"), 1}, {const_cast<char*>("23"), 2}}};
  if (take(co_await faio::io::writev(handle.get(), output.data(), 2)) != 3
      || os_file_position(handle.get()) != 6)
    co_return false;
  set_os_file_position(handle.get(), 1);
  std::array<char, 5> second{};
  std::array<iovec, 2> input{{{second.data(), 2}, {second.data() + 2, 3}}};
  if (take(co_await faio::io::readv(handle.get(), input.data(), 2)) != 5
      || std::string_view(second.data(), 5) != "XY123" || os_file_position(handle.get()) != 6)
    co_return false;
  if (take(co_await faio::io::read(handle.get(), first.data(), 3)) != 0
      || take(co_await faio::io::readv(handle.get(), nullptr, 0)) != 0
      || take(co_await faio::io::write(handle.get(), nullptr, 0)) != 0
      || os_file_position(handle.get()) != 6)
    co_return false;
  set_os_file_position(handle.get(), 4);
  std::array<char, 6> partial{};
  std::array<iovec, 2> partial_vectors{{{partial.data(), 3}, {partial.data() + 3, 3}}};
  if (take(co_await faio::io::readv(handle.get(), partial_vectors.data(), 2)) != 2
      || std::string_view(partial.data(), 2) != "23" || os_file_position(handle.get()) != 6)
    co_return false;
  auto invalid = co_await faio::io::read(handle.get(), nullptr, 1);
  if (invalid || invalid.error().value() != EFAULT || os_file_position(handle.get()) != 6)
    co_return false;
  take(co_await faio::io::close(handle.release()));  // 驱动先撤销 IOCP 句柄缓存，再真实关闭。

  // 同步外部 HANDLE 也必须借用 OS 游标，不能要求调用者先改成 OVERLAPPED 打开。
  faio::io::windows::owned_file_handle borrowed{
      ::CreateFileW(name.c_str(),
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr)};
  if (!borrowed)
    throw std::runtime_error("borrowed synchronous file open failed");
  set_os_file_position(borrowed.get(), 2);
  if (take(co_await faio::io::write(borrowed.get(), "!", 1)) != 1
      || os_file_position(borrowed.get()) != 3)
    co_return false;
  set_os_file_position(borrowed.get(), 0);
  if (take(co_await faio::io::read(borrowed.get(), first.data(), 3)) != 3
      || std::string_view(first.data(), 3) != "qX!" || os_file_position(borrowed.get()) != 3)
    co_return false;
  co_return true;
}

TEST_P(WindowsFilesystem, RawDefaultScalarAndVectorsUseOsCursorAndPreserveExplicitOffsets) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(raw_default_cursor_contract(directory.path / L"低层游标.bin")));
}

/** @brief 追加权限由真实 HANDLE 查询；外部 append-only 与 truncate+append 都保留契约。 */
faio::task<bool> raw_append_contract(std::filesystem::path name) {
  take(co_await faio::fs::write(name, std::string_view{"base"}));
  faio::io::windows::owned_file_handle handle{
      ::CreateFileW(name.c_str(),
                    FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                    nullptr)};
  if (!handle)
    throw std::runtime_error("borrowed append-only file open failed");
  set_os_file_position(handle.get(), 0);
  if (take(co_await faio::io::write(handle.get(), "+", 1)) != 1
      || os_file_position(handle.get()) != 5)
    co_return false;
  std::array<iovec, 2> vectors{{{const_cast<char*>("x"), 1}, {const_cast<char*>("y"), 1}}};
  set_os_file_position(handle.get(), 0);
  if (take(co_await faio::io::writev(handle.get(), vectors.data(), 2)) != 2
      || os_file_position(handle.get()) != 7
      || take(co_await faio::fs::read_to_string(name)) != "base+xy")
    co_return false;
  take(co_await faio::io::close(handle.release()));
  const auto utf8 = name.u8string();
  handle.reset(take(co_await faio::io::open(reinterpret_cast<const char*>(utf8.c_str()),
                                            _O_RDWR | _O_TRUNC | _O_APPEND | _O_SEQUENTIAL)));
  if (take(co_await faio::io::write(handle.get(), "new", 3)) != 3
      || os_file_position(handle.get()) != 3)
    co_return false;
  set_os_file_position(handle.get(), 0);
  if (take(co_await faio::io::writev(handle.get(), vectors.data(), 2)) != 2
      || os_file_position(handle.get()) != 5)
    co_return false;  // truncate 临时写权限必须收窄，否则这里会覆盖 new 开头。
  set_os_file_position(handle.get(), 0);
  std::array<char, 5> bytes{};
  if (take(co_await faio::io::read(handle.get(), bytes.data(), bytes.size())) != 5
      || std::string_view(bytes.data(), bytes.size()) != "newxy")
    co_return false;
  take(co_await faio::io::close(handle.release()));
  // 句柄相对 NtCreateFile 的 truncate+append 路径不能遗漏同样的权限收窄。
  faio::io::windows::owned_file_handle parent{
      ::CreateFileW(name.parent_path().c_str(),
                    FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr)};
  if (!parent)
    throw std::runtime_error("relative append parent open failed");
  handle.reset(take(co_await faio::io::openat(
      parent.get(), "relative-append", _O_RDWR | _O_CREAT | _O_TRUNC | _O_APPEND)));
  take(co_await faio::io::write(handle.get(), "a", 1));
  set_os_file_position(handle.get(), 0);
  take(co_await faio::io::write(handle.get(), "b", 1));
  take(co_await faio::io::close(handle.release()));
  co_return take(co_await faio::fs::read_to_string(name.parent_path() / "relative-append")) == "ab";
}

TEST_P(WindowsFilesystem, RawAppendOnlyAndTruncateAppendKeepAtomicEndOfFileSemantics) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(raw_append_contract(directory.path / "append")));
}

faio::task<std::size_t> raw_vector_group(HANDLE handle, char token) {
  std::string payload(128 * 1024, token);
  std::array<iovec, 2> vectors{{{payload.data(), payload.size() / 2},
                                {payload.data() + payload.size() / 2, payload.size() / 2}}};
  co_return take(co_await faio::io::writev(handle, vectors.data(), 2));
}

/** @brief DuplicateHandle 共享同一 FILE_OBJECT 游标，整个 vector 请求必须保持连续。 */
faio::task<bool> raw_duplicate_cursor_contract(std::filesystem::path name) {
  const auto utf8 = name.u8string();
  faio::io::windows::owned_file_handle handle{take(co_await faio::io::open(
      reinterpret_cast<const char*>(utf8.c_str()), _O_RDWR | _O_CREAT | _O_EXCL))};
  HANDLE duplicated{};
  if (!::DuplicateHandle(::GetCurrentProcess(),
                         handle.get(),
                         ::GetCurrentProcess(),
                         &duplicated,
                         0,
                         FALSE,
                         DUPLICATE_SAME_ACCESS))
    throw std::runtime_error("duplicate raw file handle failed");
  faio::io::windows::owned_file_handle clone{duplicated};
  auto one = faio::spawn(raw_vector_group(handle.get(), 'a'));
  auto two = faio::spawn(raw_vector_group(clone.get(), 'b'));
  const auto one_count = co_await one,
             two_count = co_await two;  // 两个借用都排空再检查/关闭 HANDLE。
  if (one_count != 128 * 1024 || two_count != 128 * 1024
      || os_file_position(handle.get()) != 256 * 1024
      || os_file_position(clone.get()) != 256 * 1024)
    co_return false;
  set_os_file_position(handle.get(), 0);
  std::string result(256 * 1024, '\0');
  if (take(co_await faio::io::read(handle.get(), result.data(), result.size())) != result.size())
    co_return false;
  const std::string first(128 * 1024, 'a'), second(128 * 1024, 'b');
  const bool grouped = result == first + second || result == second + first;
  take(co_await faio::io::close(clone.release()));
  take(co_await faio::io::close(handle.release()));
  co_return grouped;
}

TEST_P(WindowsFilesystem, RawDuplicateHandlesSerializeWholeVectoredOsCursorOperations) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(raw_duplicate_cursor_contract(directory.path / "duplicate")));
}

/** @brief 非法 UTF-8、未知 flag 和转换异常必须交付失败，文件 lane 不可 terminate。 */
faio::task<bool> raw_path_validation_contract(std::filesystem::path name) {
  const std::string invalid_utf8{"\xc0\xaf"};  // 过长编码不是合法 UTF-8。
  const auto invalid = co_await faio::io::open(invalid_utf8.c_str(), _O_RDONLY);
  if (invalid || invalid.error().domain() != faio::error_domain::win32
      || invalid.error().value() != ERROR_NO_UNICODE_TRANSLATION)
    co_return false;
  faio::io::detail::io_request request;
  request.path = std::string{"prefix\0suffix", 13};
  if (faio::io::windows::open_file(request) != -EINVAL)
    co_return false;
  const auto utf8 = name.u8string();
  const auto text =
      co_await faio::io::open(reinterpret_cast<const char*>(utf8.c_str()), _O_RDWR | _O_TEXT);
  const auto unknown =
      co_await faio::io::open(reinterpret_cast<const char*>(utf8.c_str()), _O_RDONLY | 0x40000000);
  const auto mode =
      co_await faio::io::open(reinterpret_cast<const char*>(utf8.c_str()), _O_WRONLY | _O_RDWR);
  if (text || text.error().value() != EOPNOTSUPP || unknown || unknown.error().value() != EOPNOTSUPP
      || mode || mode.error().value() != EINVAL || std::filesystem::exists(name))
    co_return false;
  faio::io::windows::owned_file_handle temporary{
      take(co_await faio::io::open(reinterpret_cast<const char*>(utf8.c_str()),
                                   _O_RDWR | _O_CREAT | _O_EXCL | _O_TEMPORARY | _O_SHORT_LIVED))};
  take(co_await faio::io::write(temporary.get(), "temporary", 9));
  take(co_await faio::io::close(temporary.release()));
  if (std::filesystem::exists(name))
    co_return false;
  const auto message = faio::io::windows::format_windows_error(
      faio::io::windows::make_windows_error(ERROR_ACCESS_DENIED));
  const auto winsock =
      faio::io::windows::format_windows_error(faio::io::windows::make_winsock_error(WSAECONNRESET));
  co_return !message.empty() && !winsock.empty()
      && ::MultiByteToWideChar(CP_UTF8,
                               MB_ERR_INVALID_CHARS,
                               message.data(),
                               static_cast<int>(message.size()),
                               nullptr,
                               0)
             > 0
      && faio::io::windows::format_windows_error(faio::make_error(EINVAL))
             == faio::make_error(EINVAL).message();
}

TEST_P(WindowsFilesystem, RawUtf8OpenValidationTemporaryFlagsAndNativeErrorFormatting) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  EXPECT_TRUE(runtime.block_on(raw_path_validation_contract(directory.path / "temporary")));
}

/** @brief 返回存活 File/ReadDir，再停机仍必须关闭全部原生资源。 */
faio::task<std::pair<faio::fs::File, faio::fs::ReadDir>> live_resources(
    std::filesystem::path root) {
  auto file = take(co_await faio::fs::File::create(root / "live"));
  take(co_await file.write_all(std::span<const char>{"alive", 5}));
  auto directory = take(co_await faio::fs::read_dir(root));
  co_return std::pair{std::move(file), std::move(directory)};
}

TEST_P(WindowsFilesystem, ShutdownClosesResourcesThatOutliveRuntime) {
  temporary_directory directory;
  runtime_context runtime{config(GetParam())};
  auto resources = runtime.block_on(live_resources(directory.path));
  const HANDLE handle = resources.first.native_handle();
  runtime.shutdown(faio::io::shutdown_policy::cancel_all);
  EXPECT_EQ(resources.first.native_handle(), INVALID_HANDLE_VALUE);
  DWORD flags{};
  EXPECT_FALSE(::GetHandleInformation(handle, &flags));
  EXPECT_EQ(::GetLastError(), ERROR_INVALID_HANDLE);
}

INSTANTIATE_TEST_SUITE_P(RuntimeModes,
                         WindowsFilesystem,
                         testing::Values(faio::runtime::mode::current_thread,
                                         faio::runtime::mode::multi_thread));
}  // namespace
