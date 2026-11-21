/**
 * @file faio/detail/io/engine.hpp
 * @brief IO 引擎拥有者。
 */
#pragma once
#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/io/core/driver_ref.hpp"
#include "faio/detail/io/core/submitter_ref.hpp"
#if defined(_WIN32)
#include <expected>
#include <system_error>
#endif
namespace faio::io {
/**  @brief 平台无关的 owning 引擎；纯头文件，不要求具体 runtime 或调度器。 */
class io_engine {
 public:
  /** @brief 构造 I/O 提交与驱动引擎。 */
  explicit io_engine(engine_config config = {})
      : domain_(std::make_shared<detail::io_domain>(std::move(config))) {}
#if defined(_WIN32)
  /**
   *  @brief 保留 Windows 旧入口的无异常工厂；成功时返回已初始化的 IOCP 引擎。
   * @param config 域槽、资源和隔离服务的有界配置。
   * @return 初始化成功且持有资源所有权的引擎；错误保留系统代码，不降级后端。
   */
  static std::expected<io_engine, std::error_code> create(engine_config config = {}) noexcept {
    try {
      return io_engine{std::move(config)};
    } catch (const std::system_error& error) {
      return std::unexpected{error.code()};
    } catch (const std::invalid_argument&) {
      return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    } catch (...) {
      return std::unexpected{std::make_error_code(std::errc::not_enough_memory)};
    }
  }
#endif
  /** @brief 禁止复制构造 I/O 提交与驱动引擎。 */
  io_engine(const io_engine&) = delete;

  /** @brief 禁止复制赋值。 */
  io_engine& operator=(const io_engine&) = delete;

  /** @brief 移动构造 I/O 提交与驱动引擎。 */
  io_engine(io_engine&&) = default;

  /** @brief 移动赋值。 */
  io_engine& operator=(io_engine&& other) noexcept {
    if (this != &other) {
      shutdown();
      domain_ = std::move(other.domain_);
    }
    return *this;
  }

  /** @brief 销毁 I/O 提交与驱动引擎。 */
  ~io_engine() { shutdown(); }

  /** @brief 取得该对象关联的 I/O 上下文租约。 */
  io_context context() const noexcept { return io_context{domain_}; }

  /** @brief 取得 I/O 事件驱动接口。 */
  io_driver_ref driver() const noexcept {
    return domain_ ? io_driver_ref{*domain_} : io_driver_ref{};
  }

  /** @brief 取得 I/O 请求提交接口。 */
  io_submitter_ref submitter() const noexcept {
    return domain_ ? io_submitter_ref{*domain_} : io_submitter_ref{};
  }

  /** @brief 返回实际已初始化后端提供的能力快照。 */
  io_capabilities capabilities() const noexcept {
    return domain_ ? domain_->capabilities() : io_capabilities{};
  }

  /** @brief 读取后端累计的提交、完成和控制事件统计。 */
  detail::backend_statistics statistics() const noexcept {
    return domain_ ? domain_->statistics() : detail::backend_statistics{};
  }

  /** @brief 停止接受新业务，按停止策略取消或保留已接受请求并启动排空。 */
  void begin_shutdown(shutdown_policy policy = shutdown_policy::cancel_all) noexcept {
    if (domain_)
      domain_->begin_shutdown(policy);
  }

  /** @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。 */
  void shutdown() noexcept {
    if (domain_)
      domain_->shutdown();
  }

  /**  @brief 临时安装 IO TLS；仅提供默认上下文，资源永远保存独立归属 lease。 */
  class binding {
   public:
    /** @brief 构造 I/O 驱动的作用域绑定。 */
    explicit binding(const io_engine& engine) noexcept
        : previous_(::faio::detail::current_thread_context().io.domain) {
      ::faio::detail::current_thread_context().io.domain = engine.domain_.get();
    }

    /** @brief 销毁 I/O 驱动的作用域绑定。 */
    ~binding() {
      if (auto* context = ::faio::detail::try_current_thread_context())
        context->io.domain = previous_;
    }

    /** @brief 禁止复制构造 I/O 驱动的作用域绑定。 */
    binding(const binding&) = delete;

   private:
    detail::io_domain* previous_; /**< 进入当前作用域前保存的线程绑定或任务上下文，析构时恢复。 */
  };

 private:
  std::shared_ptr<detail::io_domain>
      domain_; /**< 共享持有所属 I/O 域，使其在取消回调和完成发布期间保持有效。 */
};
}  // namespace faio::io

#include "faio/detail/runtime/common/coroutine_integration.hpp"
