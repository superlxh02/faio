#pragma once
#include "faio/detail/execution/blocking_executor.hpp"
#include <chrono>
#include <memory>
#include <stdexcept>

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

  /** @brief 所有权仅在接受成功后交给服务；失败供 start_blocking
   * 回滚根任务计数。 */
  void submit(::faio::move_only_function<void()> job) {
    auto admitted = service_->try_submit(std::move(job));  // 短锁中判断容量并移动任务。
    if (!admitted && admitted.error().value() == ECANCELED)
      throw std::logic_error("阻塞线程池已关闭");
    if (!admitted)  // 不接受时池中没有任务引用，允许上层立即释放完成状态。
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
}  // namespace faio::runtime::detail
