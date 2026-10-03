#pragma once
#include "faio/detail/io/backends/iocp/backend.hpp"
#include <atomic>
#include <memory>
#include <stop_token>
namespace faio::io {
/** @brief Windows 完成驱动框架；网络/文件实现将在独立阶段接入。 */
enum class shutdown_policy : std::uint8_t { drain, cancel_all };
struct drive_budget {
  std::size_t max_events{256}, max_completions{256};
};
struct drive_result {
  std::size_t completions{};
  bool progressed{}, more_work{};
  int fatal_error{static_cast<int>(windows::call_not_implemented)};
};
struct io_capabilities {
  bool network{}, readiness{}, vectored{}, filesystem{}, native_filesystem{},
      zero_copy{}, migration{};
  const char *backend{"iocp-unavailable"};
};
struct engine_config {};
namespace windows {
/** @brief context 仅保持控制域生命周期，backend 未实现时不允许接受操作。 */
struct framework_domain {
  std::atomic<bool> stopped{};
  std::stop_source stop;
};
} // namespace windows
class io_context {
public:
  io_context() = default;
  explicit io_context(
      std::shared_ptr<windows::framework_domain> domain) noexcept
      : domain_(std::move(domain)) {}
  static io_context current() noexcept { return {}; }
  bool valid() const noexcept { return !!domain_; }
  explicit operator bool() const noexcept { return valid(); }
  bool stopped() const noexcept {
    return !domain_ || domain_->stopped.load(std::memory_order_acquire);
  }
  std::stop_token stop_token() const noexcept {
    return domain_ ? domain_->stop.get_token() : std::stop_token{};
  }

private:
  std::shared_ptr<windows::framework_domain> domain_;
};
class io_driver_ref {
public:
  drive_result drive(drive_budget = {}) const noexcept { return {}; }
  drive_result wait_and_drive(std::optional<int> timeout = {},
                              drive_budget = {}) const noexcept {
    return {};
  }
  void wake() const noexcept {}
  bool quiescent() const noexcept { return true; }
};
/** @brief owning façade 允许查询明确能力；create() 返回未实现错误防止误部署。
 */
class io_engine {
public:
  explicit io_engine(engine_config = {})
      : domain_(std::make_shared<windows::framework_domain>()) {}
  io_engine(const io_engine &) = delete;
  io_engine &operator=(const io_engine &) = delete;
  io_engine(io_engine &&) = default;
  ~io_engine() { shutdown(); }
  static std::expected<io_engine, std::error_code>
  create(engine_config = {}) noexcept {
    return std::unexpected{windows::unavailable_error()};
  }
  io_context context() const noexcept { return io_context{domain_}; }
  io_driver_ref driver() const noexcept { return {}; }
  io_capabilities capabilities() const noexcept { return {}; }
  void begin_shutdown(shutdown_policy = shutdown_policy::cancel_all) noexcept {
    if (domain_ && !domain_->stopped.exchange(true, std::memory_order_acq_rel))
      domain_->stop.request_stop();
  }
  void shutdown() noexcept { begin_shutdown(); }

private:
  std::shared_ptr<windows::framework_domain> domain_;
};
inline io_context current_context() noexcept { return io_context::current(); }
} // namespace faio::io
