/**
 * @file faio/detail/runtime/default.hpp
 * @brief 默认运行时服务。
 */
#ifndef FAIO_DETAIL_RUNTIME_DEFAULT_HPP
#define FAIO_DETAIL_RUNTIME_DEFAULT_HPP

#include "faio/detail/runtime/context.hpp"
#include "faio/detail/log/logger.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace faio::runtime::detail {
/**
 * @brief 进程默认运行时。配置只允许在第一次使用前修改；外部入口用共享锁保护
 * 提交与 shutdown 的竞争。worker 内部持有 scheduler_ref，不经过此锁。
 */
class default_runtime_service {
 public:
  /** @brief 构造进程默认运行时服务。 */
  default_runtime_service() = default;

  /** @brief 禁止复制构造进程默认运行时服务。 */
  default_runtime_service(const default_runtime_service&) = delete;

  /** @brief 禁止复制赋值。 */
  default_runtime_service& operator=(const default_runtime_service&) = delete;

  /** @brief 配置运行时服务。 */
  void configure(runtime_config config) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 上配置运行时");
    config = validate_config(config);
    std::unique_lock lock(mutex_);
    if (context_ || stopped_)
      throw std::logic_error("运行时已启动或已关闭");
    config_ = std::move(config);
  }

  /** @brief 取得或惰性创建默认运行时上下文，并在共享锁保护下调用 action。 */
  template <class F>
  decltype(auto) with_context(F&& action) {
    std::shared_lock lock(mutex_);
    if (!context_) {
      lock.unlock();
      std::unique_lock init_lock(mutex_);
      if (stopped_)
        throw std::logic_error("默认运行时已关闭");
      if (!context_)
        context_ = std::make_unique<runtime_context>(config_);
      init_lock.unlock();
      lock.lock();
    }
    if (stopped_)
      throw std::logic_error("默认运行时已关闭");
    return std::invoke(std::forward<F>(action), *context_);
  }

  /** @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。 */
  void shutdown(io::shutdown_policy policy = io::shutdown_policy::drain) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 上关闭运行时");
    std::unique_lock lock(mutex_);
    if (stopped_)
      return;
    auto driver_affinity = [this]() -> std::optional<worker_affinity_scope> {
      if (context_)
        return context_->prepare_driver_affinity();
      return {};
    }();
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    if (context_)
      context_->external_host().begin_quiescing();
#endif
    stopped_ = true;
    auto previous = std::move(context_);
    lock.unlock();
    if (previous)
      previous->stop_prebound(policy);
    previous.reset();
  }

 private:
  std::shared_mutex mutex_; /**< 外部提交持共享锁，首次初始化/配置/关闭持独占锁。 */
  runtime_config config_{}; /**< 首次启动之前可修改的默认配置。 */
  std::unique_ptr<runtime_context> context_; /**< 惰性创建并独占拥有的默认运行时。 */
  bool stopped_{};                           /**< 关闭后置位，禁止重新创建默认运行时。 */
};

/** @brief 取得进程默认运行时服务。 */
inline default_runtime_service& default_service() {
  (void)::faio::log::logger();
  static default_runtime_service service;
  return service;
}

/** @brief 在进程默认运行时上下文中调用函数。 */
template <class F>
decltype(auto) with_default_runtime(F&& action) {
  return default_service().with_context(std::forward<F>(action));
}
}  // namespace faio::runtime::detail

namespace faio::runtime {
/** @brief 首次 block_on/spawn 前配置默认运行时；未配置时使用 runtime::config 默认值。 */
inline void configure(config options) {
  detail::default_service().configure(std::move(options));
}

/** @brief 程序需要确定性退出时调用；关闭后不再允许提交或重新启动默认运行时。 */
inline void shutdown(io::shutdown_policy policy = io::shutdown_policy::drain) {
  detail::default_service().shutdown(policy);
}
}  // namespace faio::runtime

#endif
