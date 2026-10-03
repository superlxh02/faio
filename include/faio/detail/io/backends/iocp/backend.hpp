#pragma once
#include "faio/detail/io/backend_protocol.hpp"
#include "faio/detail/io/backends/iocp/overlapped_state.hpp"
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
namespace faio::io::windows {
/**
 * @brief IOCP 是原生 Proactor，网络与文件共用 OVERLAPPED 完成管线。
 * @details native_filesystem_supported 表示平台模型具备原生异步文件能力；
 *          implemented 表示本发行适配状态。当前只搭建框架，不接受任何实际 IO。
 */
struct iocp_capabilities {
  static constexpr bool native_proactor = true;
  static constexpr bool native_filesystem_supported = true;
  static constexpr bool implemented = false;
  static constexpr bool overlapped_network = false, overlapped_files = false,
                        readiness = false;
};
inline constexpr std::uint32_t call_not_implemented =
    120; ///< ERROR_CALL_NOT_IMPLEMENTED。
inline std::error_code unavailable_error() noexcept {
  return {static_cast<int>(call_not_implemented), std::system_category()};
}
// 所有平台使用同一个函数表；HANDLE/SOCKET 通过带种类的 uintptr_t 完整保存。
using backend_box = ::faio::io::detail::backend_box;
using native_registration = ::faio::io::detail::native_registration;
using submit_status = ::faio::io::detail::backend_submit_status;
using submit_result = ::faio::io::detail::backend_submit_result;
/** @brief IOCP 框架遵守中立生命周期协议，但明确在接受前拒绝实际请求。 */
class unavailable_backend {
public:
  static constexpr bool native_proactor = true;
  static constexpr const char *backend_name = "iocp-unavailable";
  const char *name() const noexcept { return backend_name; }
  /// @brief 当前发行未适配 IOCP，接受前明确拒绝，原 HANDLE/SOCKET
  /// 仍由调用方拥有。
  int attach(native_registration, std::uint64_t) noexcept {
    return -static_cast<int>(call_not_implemented);
  }
  void detach(native_registration) noexcept {}
  /// @brief 返回未实现而非虚假 accepted，因而不会保留任何 OVERLAPPED/payload
  /// 引用。
  submit_result try_submit(::faio::io::detail::backend_operation) noexcept {
    return {submit_status::rejected, static_cast<int>(call_not_implemented)};
  }
  ::faio::io::detail::backend_flush_result flush() noexcept { return {}; }
  /// @brief 无真实 IOCP 消费循环，不用 readiness 或 helper 线程替代完成包。
  int poll(std::span<::faio::io::detail::backend_event>,
           std::optional<int>) noexcept {
    return -static_cast<int>(call_not_implemented);
  }
  void request_cancel(std::uint64_t) noexcept {}
  void wake() noexcept {}
  void begin_shutdown() noexcept {}
  /// @brief 因所有请求均在接受前拒绝，框架后端没有任何原生引用需要排空。
  bool quiescent() const noexcept { return true; }
  /// @brief 平台 Proactor 模型与本发行实际能力分开，当前不宣称支持任何请求。
  bool supports(std::uint32_t) const noexcept { return false; }
  ::faio::io::detail::backend_statistics statistics() const noexcept {
    return {};
  }
};
/** @brief factory 在完成实际 IOCP 适配以前明确返回 Windows 错误。 */
inline std::expected<backend_box, std::error_code>
make_iocp_backend() noexcept {
  return std::unexpected{unavailable_error()};
}
/** @brief 合同 smoke 使用同一 backend_box，验证拒绝阶段没有原生引用。 */
inline backend_box make_unavailable_backend_box() {
  return backend_box{std::make_unique<unavailable_backend>()};
}
} // namespace faio::io::windows
