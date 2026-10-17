/**
 * @file faio/detail/io/engine.hpp
 * @brief 跨平台异步 IO：IO 引擎拥有者。
 * @details 统一 IO 通过上下文、资源租约与等待器连接运行时；组合操作保留部分进度并在安全完成后归还负载及方向执行权。
 */
#pragma once
#include "faio/detail/io/core/driver_ref.hpp"
#include "faio/detail/io/core/submitter_ref.hpp"
#if defined(_WIN32)
#include <expected>
#include <system_error>
#endif
namespace faio::io {
/** @brief 平台无关 owning 引擎；纯头文件，不要求具体 runtime 或调度器。 */
class io_engine {
 public:
  explicit io_engine(engine_config config = {})
      : domain_(std::make_shared<detail::io_domain>(std::move(config))) {}
#if defined(_WIN32)
  /** @brief 保留 Windows 旧入口的无异常工厂，成功返回真正可工作的 IOCP 引擎。
   * @param config 域槽、资源和隔离服务的有界配置。
   * @return 初始化成功的拥有型引擎；错误保留系统代码，不降级后端。
   */
  static std::expected<io_engine, std::error_code> create(engine_config config = {}) noexcept {
    try {
      /// @brief 只创建一次完成端口和稳定池，返回值移动不复制它们。
      return io_engine{std::move(config)};
    } catch (const std::system_error& error) {
      /// @brief Win32/Winsock 初始化原因原样传递。
      return std::unexpected{error.code()};
    } catch (const std::invalid_argument&) {
      return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    } catch (...) {
      return std::unexpected{std::make_error_code(std::errc::not_enough_memory)};
    }
  }
#endif
  io_engine(const io_engine&) = delete;

  io_engine& operator=(const io_engine&) = delete;

  io_engine(io_engine&&) = default;

  io_engine& operator=(io_engine&& other) noexcept {
    if (this != &other) {
      shutdown();
      domain_ = std::move(other.domain_);
    }
    return *this;
  }

  ~io_engine() { shutdown(); }

  /// @brief 取得当前对象关联的 IO 上下文租约。
  io_context context() const noexcept { return io_context{domain_}; }

  io_driver_ref driver() const noexcept {
    return domain_ ? io_driver_ref{*domain_} : io_driver_ref{};
  }

  io_submitter_ref submitter() const noexcept {
    return domain_ ? io_submitter_ref{*domain_} : io_submitter_ref{};
  }

  /// @brief 返回实际已初始化后端提供的能力快照。
  io_capabilities capabilities() const noexcept {
    return domain_ ? domain_->capabilities() : io_capabilities{};
  }

  /// @brief 读取后端累计提交、完成与控制事件统计。
  detail::backend_statistics statistics() const noexcept {
    return domain_ ? domain_->statistics() : detail::backend_statistics{};
  }

  /// @brief 停止接受新业务，按停止策略取消或保留已接受请求并启动排空。
  void begin_shutdown(shutdown_policy policy = shutdown_policy::cancel_all) noexcept {
    if (domain_)
      domain_->begin_shutdown(policy);
  }

  /// @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。
  void shutdown() noexcept {
    if (domain_)
      domain_->shutdown();
  }

  /** @brief 临时安装 IO TLS；仅提供默认上下文，资源永远保存独立归属 lease。 */
  class binding {
   public:
    explicit binding(const io_engine& engine) noexcept : previous_(detail::current_domain) {
      detail::current_domain = engine.domain_.get();
    }

    ~binding() { detail::current_domain = previous_; }

    binding(const binding&) = delete;

   private:
    detail::io_domain* previous_;  ///< 进入当前作用域前保存的线程绑定或任务上下文，析构时恢复。
  };

 private:
  std::shared_ptr<detail::io_domain> domain_;  ///< 共享持有所属 IO 域，覆盖取消回调与完成发布。
};
}
