/**
 * @file faio/detail/runtime/common/blocking_pool.hpp
 * @brief 通用阻塞任务池。
 */
#pragma once
#include "faio/detail/blocking/service.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include <chrono>
#include <concepts>
#include <cstddef>
#include <format>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace faio::runtime::detail {
/**
 *  @brief 用户阻塞任务的有界服务适配器，与文件及 DNS 服务隔离。
 * @details 固定池在运行时构造阶段预先启动所有线程。弹性池按需启动线程，空闲超过
 * keep_alive 时回收。队列满载时提交立即报错。
 */
class blocking_pool {
 public:
  /** @brief 按指定策略初始化阻塞服务。 */
  blocking_pool(std::size_t limit,
                std::chrono::milliseconds keep_alive,
                std::size_t queue_limit = 4096,
                blocking_pool_policy policy = blocking_pool_policy::ELASTIC) {
    if (keep_alive.count() <= 0)
      throw std::invalid_argument("阻塞线程保活参数无效");
    service_ = policy == blocking_pool_policy::ELASTIC
                   ? std::make_shared<blocking::service>(limit, queue_limit, keep_alive)
                   : std::make_shared<blocking::service>(limit, queue_limit);
  }

  /** @brief 禁止复制构造阻塞任务执行池。 */
  blocking_pool(const blocking_pool&) = delete;

  /** @brief 禁止复制赋值。 */
  blocking_pool& operator=(const blocking_pool&) = delete;

  /** @brief 销毁阻塞任务执行池。 */
  ~blocking_pool() { close(); }

  /**
   *  @brief 成功接纳后由服务负责执行；失败时由 submit_blocking_task 回滚根任务计数。
   * @details 参数转移遵循 blocking::service::try_submit 的小对象/大对象规则。
   */
  template <class F>
    requires std::invocable<std::decay_t<F>&> && std::constructible_from<std::decay_t<F>, F&&>
  void submit(F&& job) {
    auto admitted = service_->try_submit(std::forward<F>(job));
    if (!admitted && admitted.error().value() == ECANCELED)
      throw std::logic_error("阻塞线程池已关闭");
    if (!admitted)
      throw std::runtime_error(std::format("阻塞任务提交失败：{}", admitted.error()));
  }

  /**  @brief 保持调度器存活时排空任务；只能从池外的生命周期控制线程调用。 */
  void close() noexcept { service_->close(); }

  /**  @brief 与文件系统提供者共用的独立服务协议。 */
  blocking::service_ref service() const noexcept { return blocking::service_ref{service_}; }

 private:
  std::shared_ptr<blocking::service> service_; /**< 服务租约的生命周期覆盖所有已接纳任务。 */
};
}  // namespace faio::runtime::detail
