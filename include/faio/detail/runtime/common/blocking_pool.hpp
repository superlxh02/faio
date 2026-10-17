/**
 * @file faio/detail/runtime/common/blocking_pool.hpp
 * @brief 运行时调度、线程协调与定时驱动：通用阻塞任务池。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#pragma once
#include "faio/detail/execution/blocking_executor.hpp"
#include <chrono>
#include <concepts>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace faio::runtime::detail {
/** @brief 用户阻塞任务的有界服务适配器，与文件及 DNS 服务隔离。
 * @details 固定线程在 runtime 构造阶段预热，submit 不创建线程、回收线程或
 * join。keep_alive 保留配置兼容性；固定池仅在 close 时回收，避免低时延路径
 * 遇到线程创建。队列满载明确报告错误，不在 IO worker 同步等容量。
 */
class blocking_pool {
 public:
  /** @brief 构造预热线程与有界队列，初始化失败不接受任何任务。 */
  blocking_pool(std::size_t limit,
                std::chrono::milliseconds keep_alive,
                std::size_t queue_limit = 4096)
      : service_(std::make_shared<execution::blocking_executor>(limit, queue_limit)) {
    if (keep_alive.count() <= 0)
      throw std::invalid_argument("阻塞线程保活参数无效");
  }

  blocking_pool(const blocking_pool&) = delete;

  blocking_pool& operator=(const blocking_pool&) = delete;

  ~blocking_pool() { close(); }

  /** @brief 接受成功后服务负责执行；失败供 start_blocking 回滚根任务计数。
   * @details 参数转移遵循 blocking_executor::try_submit 的小对象/大对象规则。
   */
  template <class F>
    requires std::invocable<std::decay_t<F>&> && std::constructible_from<std::decay_t<F>, F&&>
  void submit(F&& job) {
    /// @brief 接受后直接在队列构造包装。
    auto admitted = service_->try_submit(std::forward<F>(job));
    if (!admitted && admitted.error().value() == ECANCELED)
      throw std::logic_error("阻塞线程池已关闭");
    /// @brief 不接受时池中没有任务引用，允许上层立即释放完成状态。
    if (!admitted)
      throw std::runtime_error(std::format("阻塞任务提交失败：{}", admitted.error()));
  }

  /** @brief 保持调度器存活时排空任务；只能从池外的生命周期控制线程调用。 */
  void close() noexcept { service_->close(); }

  /** @brief 与 filesystem provider 相同的中立服务协议。 */
  execution::blocking_executor_ref executor() const noexcept {
    return execution::blocking_executor_ref{service_};
  }

 private:
  std::shared_ptr<execution::blocking_executor> service_;  ///< 服务 lease 覆盖所有已接受任务。
};
}
