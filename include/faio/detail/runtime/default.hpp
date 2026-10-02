#ifndef FAIO_DETAIL_RUNTIME_DEFAULT_HPP
#define FAIO_DETAIL_RUNTIME_DEFAULT_HPP

#include "faio/detail/runtime/context.hpp"
#include "faio/log.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace faio::runtime::detail {
// 进程默认运行时。配置只允许在第一次使用前修改；外部入口用共享锁保护
// 提交与 shutdown 的竞争。worker 内部持有 scheduler_ref，不经过此锁。
class default_runtime_service {
public:
  default_runtime_service() = default;
  default_runtime_service(const default_runtime_service&) = delete;
  default_runtime_service& operator=(const default_runtime_service&) = delete;

  void configure(runtime_config config) {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 上配置运行时");
    config = validate_config(config);
    std::unique_lock lock(mutex_);
    if (context_ || stopped_) throw std::logic_error("运行时已启动或已关闭");
    config_ = config;
  }

  template <class F> decltype(auto) with_context(F&& action) {
    std::shared_lock lock(mutex_);
    if (!context_) {
      lock.unlock();
      std::unique_lock init_lock(mutex_);
      if (stopped_) throw std::logic_error("默认运行时已关闭");
      if (!context_) context_ = std::make_unique<runtime_context>(config_);
      init_lock.unlock();
      lock.lock();
    }
    if (stopped_) throw std::logic_error("默认运行时已关闭");
    return std::invoke(std::forward<F>(action), *context_);
  }

  void shutdown() {
    if (::faio::detail::on_runtime_worker())
      throw std::logic_error("不能在 worker 上关闭运行时");
    std::unique_ptr<runtime_context> previous;
    {
      std::unique_lock lock(mutex_);
      if (stopped_) return;
      stopped_ = true;
      previous = std::move(context_);
    }
    // 允许已提交任务继续运行；析构会等待所有根协程退出后关闭 worker。
    previous.reset();
  }

private:
  std::shared_mutex mutex_; // 外部提交持共享锁，首次初始化/配置/关闭持独占锁。
  runtime_config config_{}; // 首次启动之前可修改的默认配置。
  std::unique_ptr<runtime_context> context_; // 惰性创建并独占拥有的默认运行时。
  bool stopped_{}; // 关闭后置位，禁止重新创建默认运行时。
};

inline default_runtime_service& default_service() {
  // 先构造 logger，使其在默认运行时完成静态析构与 worker 排空后再析构。
  (void)::faio::log::logger();
  static default_runtime_service service;
  return service;
}
} // namespace faio::runtime::detail

namespace faio::runtime {
using config = detail::runtime_config;
// 保留原公开类型别名；新代码可以使用下划线命名的 runtime::config。
using Config = config;
// 首次 block_on/spawn 前配置默认运行时；未配置时使用 runtime::config 默认值。
inline void configure(config options) { detail::default_service().configure(options); }
// 程序需要确定性退出时调用；关闭后不再允许提交或重新启动默认运行时。
inline void shutdown() { detail::default_service().shutdown(); }
} // namespace faio::runtime

#endif
