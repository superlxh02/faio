/**
 * @file faio/detail/runtime/static.hpp
 * @brief 编译期固定调度模式的运行时入口。
 */
#ifndef FAIO_DETAIL_RUNTIME_STATIC_HPP
#define FAIO_DETAIL_RUNTIME_STATIC_HPP

#include "faio/detail/runtime/context.hpp"
#include <utility>

namespace faio::runtime {
/**
 * @brief 两种驱动模式共享同一具体调度器；Mode 只决定执行线程的组织方式。
 * 默认 task 与同步对象均直接调用该调度域，不包含运行期调度分派表。
 */
template <mode Mode>
class basic_runtime {
  static_assert(Mode == mode::current_thread || Mode == mode::multi_thread);

 public:
  using scheduler_type = default_scheduler;
  using config_type = detail::runtime_config;

  /** @brief 构造静态运行时。 */
  explicit basic_runtime(config_type config = {}) : context_(fixed_mode(config)) {}

  /** @brief 禁止复制构造静态运行时。 */
  basic_runtime(const basic_runtime&) = delete;
  /** @brief 禁止复制赋值。 */
  basic_runtime& operator=(const basic_runtime&) = delete;

  /** @brief 取得绑定的调度器。 */
  basic_scheduler_ref<scheduler_type> scheduler() { return context_.scheduler(); }

  /** @brief 驱动并同步等待根任务结果。 */
  template <class T>
  T block_on(task<T, scheduler_type> child) {
    return context_.block_on(std::move(child));
  }

  /** @brief 提交并发任务并返回结果句柄。 */
  template <class T>
  join_handle<T, scheduler_type> spawn(task<T, scheduler_type> child) {
    return context_.spawn_observed(std::move(child));
  }

  /** @brief 提交已准备的工作。 */
  template <class T>
  void submit(task<T, scheduler_type> child) {
    context_.submit(std::move(child));
  }

  /** @brief 等待全部已登记任务完成。 */
  template <class... Ts>
  std::tuple<Ts...> wait_all(task<Ts, scheduler_type>... children) {
    return context_.wait_all(std::move(children)...);
  }

  /** @brief 提交阻塞函数并返回异步结果句柄。 */
  template <class F>
  auto spawn_blocking(F&& function) {
    return context_.template submit_blocking<scheduler_type>(std::forward<F>(function));
  }

  /** @brief 读取运行时配置。 */
  const config_type& config() const noexcept { return context_.config(); }

  /** @brief 判断运行时是否正在运行。 */
  bool running() const noexcept { return context_.running(); }

  /** @brief 读取当前 I/O 后端能力。 */
  io::io_capabilities io_capabilities() const noexcept { return context_.io_capabilities(); }

  /** @brief 请求协作停止。 */
  void request_stop() noexcept { context_.request_stop(); }

  /** @brief 关闭指定 I/O 方向。 */
  void shutdown(io::shutdown_policy policy = io::shutdown_policy::cancel_all) {
    context_.shutdown(policy);
  }

  /** @brief 停止接纳并排空运行时工作。 */
  void stop(io::shutdown_policy policy = io::shutdown_policy::drain) { context_.stop(policy); }

 private:
  /** @brief 读取运行时固定线程模式。 */
  static config_type fixed_mode(config_type config) {
    config._mode = Mode;
    return config;
  }

  detail::runtime_context context_;
};

using current_thread_runtime = basic_runtime<mode::current_thread>;
using multi_thread_runtime = basic_runtime<mode::multi_thread>;
}  // namespace faio::runtime
#endif
