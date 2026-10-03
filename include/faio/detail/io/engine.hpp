#pragma once
#include "faio/detail/io/driver_ref.hpp"
#include "faio/detail/io/submitter_ref.hpp"
namespace faio::io {
/** @brief 平台无关 owning 引擎；纯头文件，不要求具体 runtime 或调度器。 */
class io_engine {
public:
  explicit io_engine(engine_config config = {})
      : domain_(std::make_shared<detail::io_domain>(std::move(config))) {}
  io_engine(const io_engine &) = delete;
  io_engine &operator=(const io_engine &) = delete;
  io_engine(io_engine &&) = default;
  io_engine &operator=(io_engine &&other) noexcept {
    if (this != &other) {
      shutdown();
      domain_ = std::move(other.domain_);
    }
    return *this;
  }
  ~io_engine() { shutdown(); }
  io_context context() const noexcept { return io_context{domain_}; }
  io_driver_ref driver() const noexcept {
    return domain_ ? io_driver_ref{*domain_} : io_driver_ref{};
  }
  io_submitter_ref submitter() const noexcept {
    return domain_ ? io_submitter_ref{*domain_} : io_submitter_ref{};
  }
  io_capabilities capabilities() const noexcept {
    return domain_ ? domain_->capabilities() : io_capabilities{};
  }
  detail::backend_statistics statistics() const noexcept {
    return domain_ ? domain_->statistics() : detail::backend_statistics{};
  }
  void begin_shutdown(
      shutdown_policy policy = shutdown_policy::cancel_all) noexcept {
    if (domain_)
      domain_->begin_shutdown(policy);
  }
  void shutdown() noexcept {
    if (domain_)
      domain_->shutdown();
  }
  /** @brief 临时安装 IO TLS；仅提供默认上下文，资源永远保存独立归属 lease。 */
  class binding {
  public:
    explicit binding(const io_engine &engine) noexcept
        : previous_(detail::current_domain) {
      detail::current_domain = engine.domain_.get();
    }
    ~binding() { detail::current_domain = previous_; }
    binding(const binding &) = delete;

  private:
    detail::io_domain *previous_;
  };

private:
  std::shared_ptr<detail::io_domain> domain_;
};
} // namespace faio::io
