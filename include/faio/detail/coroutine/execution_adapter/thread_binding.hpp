/**
 * @file faio/detail/coroutine/execution_adapter/thread_binding.hpp
 * @brief execution 工作宿主的线程绑定扩展，不装配宿主实现。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
namespace faio::detail {
/** @brief 外部执行图的生命周期宿主。 */
class external_work_host;

/** @brief 执行线程与外部执行宿主的绑定。 */
class execution_host_binding {
 public:
  /** @brief 取得关联的外部执行宿主。 */
  external_work_host* external_host() const noexcept { return external_host_; }

 protected:
  /** @brief 构造执行线程与外部执行宿主的绑定。 */
  explicit execution_host_binding(external_work_host* host) noexcept : external_host_(host) {}

 private:
  external_work_host* external_host_{};
};

/** @brief 外部提交与执行宿主的绑定。 */
struct submission_host_binding {
  external_work_host* external_host{};
};

/** @brief 取得指定静态执行上下文的宿主。 */
template <class execution_binding_type, class submission_binding_type>
external_work_host* execution_host_for(const execution_binding_type* execution,
                                       const submission_binding_type* submission) noexcept {
  return execution ? execution->external_host() : submission ? submission->external_host : nullptr;
}
}  // namespace faio::detail
#endif
