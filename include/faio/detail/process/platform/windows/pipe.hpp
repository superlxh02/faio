/**
 * @file faio/detail/process/platform/windows/pipe.hpp
 * @brief 受限访问的 Windows 命名管道和 HANDLE 所有权。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/process/child_stdio.hpp"
#include "faio/detail/io/platform/windows/error.hpp"
#include <bcrypt.h>
#include <sddl.h>
#include <array>

namespace faio::process::detail {
/** @brief 独占持有 Windows 句柄，并通过 CloseHandle 释放。 */
class native_handle_type {
 public:
  /** @brief 接管 Windows 句柄；默认值表示尚无待关闭资源。 */
  native_handle_type() = default;

  /** @brief 接管 Windows 句柄；默认值表示尚无待关闭资源。 */
  explicit native_handle_type(HANDLE handle) : handle_(handle) {}

  /** @brief 禁止复制 Windows 句柄，避免重复持有关闭责任。 */
  native_handle_type(const native_handle_type&) = delete;
  /** @brief Windows 句柄不支持复制赋值，以免破坏已有所有权或关联。 */
  auto operator=(const native_handle_type&) -> native_handle_type& = delete;

  /** @brief 接管句柄并清空移动源。 */
  native_handle_type(native_handle_type&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}

  /** @brief 先关闭原句柄，再接管新句柄；移动源恢复为空。 */
  auto operator=(native_handle_type&& other) noexcept -> native_handle_type& {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  /** @brief 关闭仍拥有的 Windows 句柄。 */
  ~native_handle_type() { reset(); }

  /** @brief 借用当前原生资源，不移交关闭责任。 */
  auto get() const noexcept -> HANDLE { return handle_; }

  /** @brief 移出原生资源并清空所有者，调用方接管关闭责任。 */
  auto release() noexcept -> HANDLE { return std::exchange(handle_, nullptr); }

  /** @brief 先释放原有原生资源，再接管新的资源值。 */
  void reset(HANDLE handle = nullptr) noexcept {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE)
      ::CloseHandle(handle_);
    handle_ = handle;
  }

 private:
  HANDLE handle_{}; /**< 独占 Windows 句柄，空值或 INVALID_HANDLE_VALUE 不执行关闭。 */
};

/** @brief 拥有仅允许当前用户和 SYSTEM 访问的命名管道安全描述符。 */
class pipe_security {
 public:
  /** @brief 建立尚未分配安全描述符的所有者。 */
  pipe_security() = default;
  /** @brief 禁止复制管道安全描述符，避免重复持有所有权或复制运行状态。 */
  pipe_security(const pipe_security&) = delete;
  /** @brief 管道安全描述符不支持复制赋值，以免破坏已有所有权或关联关系。 */
  pipe_security& operator=(const pipe_security&) = delete;

  /** @brief 释放 LocalAlloc 返回的安全描述符。 */
  ~pipe_security() {
    if (descriptor_)
      ::LocalFree(descriptor_);
  }

  /** @brief 根据当前用户令牌构造管道 ACL，仅授予当前用户和 SYSTEM 访问权限。 */
  auto initialize() -> expected<void> {
    HANDLE token{};
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    native_handle_type owner{token};
    DWORD size{};
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> data(size);
    if (!::GetTokenInformation(token, TokenUser, data.data(), size, &size))
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    LPWSTR sid{};
    if (!::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid))
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    std::wstring acl;
    try {
      acl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring{sid} + L")";
    } catch (...) {
      ::LocalFree(sid);
      throw;
    }
    ::LocalFree(sid);
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            acl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    return {};
  }

  /** @brief 生成引用当前安全描述符的可继承或不可继承属性值。 */
  SECURITY_ATTRIBUTES attributes(bool inherit) const noexcept {
    return {sizeof(SECURITY_ATTRIBUTES), descriptor_, inherit ? TRUE : FALSE};
  }

 private:
  PSECURITY_DESCRIPTOR descriptor_{}; /**< 由 LocalAlloc 分配并由 LocalFree 释放的安全描述符。 */
};

/** @brief 同时拥有父端异步句柄和子端阻塞句柄的管道对。 */
class native_pipe_pair {
 public:
  /** @brief 禁止复制父子两端管道，避免重复持有所有权或复制运行状态。 */
  native_pipe_pair(const native_pipe_pair&) = delete;
  /** @brief 父子两端管道不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const native_pipe_pair&) -> native_pipe_pair& = delete;
  /** @brief 移交父端、子端和方向，两端资源的关闭责任一并转移。 */
  native_pipe_pair(native_pipe_pair&&) noexcept = default;
  /** @brief 关闭本对象原有管道端，再接管父端、子端及方向。 */
  auto operator=(native_pipe_pair&&) noexcept -> native_pipe_pair& = default;
  /** @brief 由两端资源的所有者关闭尚未移交的父端和子端。 */
  ~native_pipe_pair() = default;

  /** @brief 创建方向固定的父子管道对，父端异步、子端阻塞且仅子端可继承。 */
  static auto create(bool writer) -> expected<native_pipe_pair> {
    pipe_security security;
    if (auto checked = security.initialize(); !checked)
      return std::unexpected{checked.error()};
    native_pipe_pair result{writer};
    for (int attempt{}; attempt < 8; ++attempt) {
      std::array<unsigned char, 16> random{};
      const auto status = ::BCryptGenRandom(nullptr,
                                            random.data(),
                                            static_cast<ULONG>(random.size()),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
      if (status < 0)
        return std::unexpected{make_error(EIO)};
      constexpr wchar_t hex[] = L"0123456789abcdef";
      std::wstring name = L"\\\\.\\pipe\\faio-" + std::to_wstring(::GetCurrentProcessId()) + L"-";
      for (auto byte : random) {
        name.push_back(hex[byte >> 4]);
        name.push_back(hex[byte & 15]);
      }
      auto parent_security = security.attributes(false);
      HANDLE parent = ::CreateNamedPipeW(
          name.c_str(),
          (writer ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED
              | FILE_FLAG_FIRST_PIPE_INSTANCE,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
          1,
          65536,
          65536,
          0,
          &parent_security);
      if (parent == INVALID_HANDLE_VALUE) {
        const auto e = ::GetLastError();
        if (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY)
          continue;
        return std::unexpected{io::windows::make_windows_error(e)};
      }
      result.parent_.reset(parent);
      native_handle_type event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
      if (!event.get())
        return std::unexpected{io::windows::make_windows_error(::GetLastError())};
      OVERLAPPED overlapped{};
      overlapped.hEvent = event.get();
      const BOOL connected = ::ConnectNamedPipe(parent, &overlapped);
      const DWORD connect_error = connected ? ERROR_SUCCESS : ::GetLastError();
      if (!connected && connect_error != ERROR_IO_PENDING && connect_error != ERROR_PIPE_CONNECTED)
        return std::unexpected{io::windows::make_windows_error(connect_error)};
      auto child_security = security.attributes(true);
      HANDLE child = ::CreateFileW(name.c_str(),
                                   writer ? GENERIC_READ : GENERIC_WRITE,
                                   0,
                                   &child_security,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
      if (child == INVALID_HANDLE_VALUE) {
        const auto e = ::GetLastError();
        if (connect_error == ERROR_IO_PENDING) {
          ::CancelIoEx(parent, &overlapped);
          DWORD ignored{};
          ::GetOverlappedResult(parent, &overlapped, &ignored, TRUE);
        }
        return std::unexpected{io::windows::make_windows_error(e)};
      }
      result.child_.reset(child);
      if (connect_error == ERROR_IO_PENDING) {
        DWORD ignored{};
        if (!::GetOverlappedResult(parent, &overlapped, &ignored, TRUE))
          return std::unexpected{io::windows::make_windows_error(::GetLastError())};
      }
      return result;
    }
    return std::unexpected{io::windows::make_windows_error(ERROR_PIPE_BUSY)};
  }

  /** @brief 借用供原生子进程继承的管道端。 */
  auto child_native() const noexcept -> io::detail::native_descriptor {
    return reinterpret_cast<io::detail::native_descriptor>(child_.get());
  }

  /** @brief 将父端交给指定 IO 域，仅在成功时移交关闭责任。 */
  auto take_parent(io::io_context context) -> expected<std::shared_ptr<pipe_state>> {
    auto result = pipe_state::adopt(std::move(context),
                                    reinterpret_cast<io::detail::native_descriptor>(parent_.get()),
                                    writer_);
    if (result)
      (void)parent_.release();
    return result;
  }

  /** @brief 关闭父进程持有的子端副本。 */
  void close_child() noexcept { child_.reset(); }

  /** @brief 关闭尚未交给 IO 域的父端副本。 */
  void close_parent() noexcept { parent_.reset(); }

 private:
  /** @brief 保存父端方向，管道资源由创建步骤逐项填入。 */
  explicit native_pipe_pair(bool writer) : writer_(writer) {}

  native_handle_type parent_; /**< 不允许子进程继承的 OVERLAPPED 父端句柄。 */
  native_handle_type child_;  /**< 可继承且使用同步读写的子端句柄。 */
  bool writer_;               /**< 父端是否为命名管道写端。 */
};
}  // namespace faio::process::detail
#endif
