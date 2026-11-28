/**
 * @file faio/detail/experimental/runtime_facade.hpp
 * @brief 运行时引用与拥有者。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_RUNTIME_FACADE_HPP
#define FAIO_DETAIL_EXPERIMENTAL_RUNTIME_FACADE_HPP

#include "faio/detail/experimental/runtime_options.hpp"
#include "faio/detail/runtime/context.hpp"
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace faio::experimental {
/** @brief 关联运行时与任务图的 sender 调度器。 */
template <faio::runtime::mode Mode>
class execution_scheduler;
using current_thread_scheduler = execution_scheduler<faio::runtime::mode::CURRENT_THREAD>;
using multi_thread_scheduler = execution_scheduler<faio::runtime::mode::MULTI_THREAD>;
/** @brief 可移动的 sender 句柄，负责观察已提交操作的结果。 */
template <class Tuple>
class sender_join_handle;

/** @brief 借用运行时，提供 sender 执行和任务桥接接口。 */
class runtime_ref {
 public:
  /** @brief 借用已存在的运行时上下文。 */
  explicit runtime_ref(faio::runtime::detail::runtime_context& context) noexcept
      : context_(&context) {}

  /** @brief 返回关联运行时的调度模式。 */
  auto mode() const noexcept -> faio::runtime::mode { return context_->config()._mode; }

  /** @brief 返回已初始化后端的能力快照。 */
  auto io_capabilities() const noexcept { return context_->io_capabilities(); }

  /** @brief 取得当前线程模式的调度器，模式不符时抛出异常。 */
  auto get_current_thread_scheduler() const -> current_thread_scheduler;
  /** @brief 取得多工作线程模式的调度器；模式不符时抛出异常。 */
  auto get_multi_thread_scheduler() const -> multi_thread_scheduler;
  /** @brief 同步驱动 sender，待所属任务图排空后领取结果。 */
  template <class sender_type>
  auto run(sender_type&& sender) const;
  /** @brief 将 sender 绑定到目标运行时及其任务图。 */
  template <class sender_type>
  auto bind(sender_type&& sender) const;
  /** @brief 立即提交 sender，并返回一次性结果观察句柄。 */
  template <class sender_type>
  auto spawn_sender(sender_type&& sender) const;
  /** @brief 将惰性协程的所有权交给 sender 桥接对象。 */
  template <class T>
  auto as_sender(faio::task<T> child) const;

  /** @brief 取得当前对象关联的运行时上下文引用。 */
  auto context() const noexcept -> faio::runtime::detail::runtime_context& { return *context_; }

 private:
  faio::runtime::detail::runtime_context*
      context_; /**< 借用运行时上下文，运行时必须在整个操作期间保持存活。 */
};

namespace detail {
/** @brief 具有固定调度模式的运行时所有者。 */
template <faio::runtime::mode Mode>
class runtime_owner {
 public:
  /** @brief 校验选项并创建固定模式的运行时。 */
  explicit runtime_owner(runtime_options options) : context_(checked_config(options)) {}

  /** @brief 禁止运行时所有者复制或移动。 */
  runtime_owner(const runtime_owner&) = delete;
  /** @brief 禁止复制运行时所有权。 */
  runtime_owner& operator=(const runtime_owner&) = delete;
  /** @brief 禁止运行时所有者复制或移动。 */
  runtime_owner(runtime_owner&&) = delete;
  /** @brief 禁止移动运行时所有者。 */
  runtime_owner& operator=(runtime_owner&&) = delete;

  /** @brief 取消并排空运行时；非法析构上下文触发终止。 */
  ~runtime_owner() noexcept {
    try {
      context_.shutdown(faio::io::shutdown_policy::CANCEL_ALL);
    } catch (...) {
      std::fputs("faio experimental: owner destroyed with active reservation or from worker\n",
                 stderr);
      std::terminate();
    }
  }

  /** @brief 返回借用当前运行时的执行入口。 */
  auto ref() noexcept -> runtime_ref { return runtime_ref{context_}; }

  /** @brief 返回与运行时固定模式匹配的调度器。 */
  auto get_scheduler() noexcept -> execution_scheduler<Mode>;

  /** @brief 同步驱动 sender，待所属任务图排空后领取结果。 */
  template <class sender_type>
  auto run(sender_type&& sender) {
    return ref().run(std::forward<sender_type>(sender));
  }

  /** @brief 将 sender 绑定到目标运行时及其任务图。 */
  template <class sender_type>
  auto bind(sender_type&& sender) {
    return ref().bind(std::forward<sender_type>(sender));
  }

  /** @brief 立即提交 sender，并返回一次性结果观察句柄。 */
  template <class sender_type>
  auto spawn_sender(sender_type&& sender) {
    return ref().spawn_sender(std::forward<sender_type>(sender));
  }

  /** @brief 将惰性协程的所有权交给 sender 桥接对象。 */
  template <class T>
  auto as_sender(faio::task<T> child) {
    return ref().as_sender(std::move(child));
  }

  /** @brief 向当前任务及其受控后代传播协作停止请求。 */
  void request_stop() noexcept { context_.request_stop(); }

  /** @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。 */
  void shutdown(faio::io::shutdown_policy policy = faio::io::shutdown_policy::CANCEL_ALL) {
    context_.shutdown(policy);
  }

  /** @brief 返回已初始化后端的能力快照。 */
  auto io_capabilities() const noexcept { return context_.io_capabilities(); }

 private:
  /** @brief 验证模式一致性并转换运行时配置。 */
  static auto checked_config(runtime_options options) -> faio::runtime::config {
    if (options.mode != Mode)
      throw std::invalid_argument("faio experimental: runtime owner mode mismatch");
    return to_runtime_config(options);
  }

  faio::runtime::detail::runtime_context
      context_; /**< 持有运行时上下文，维持 IO 资源归属、服务选择及安全排空所需的生命周期。 */
};
}  // namespace detail

/** @brief 由调用线程驱动的实验性运行时。 */
class current_thread_runtime final
    : public detail::runtime_owner<faio::runtime::mode::CURRENT_THREAD> {
 public:
  /** @brief 使用给定选项创建当前线程运行时。 */
  explicit current_thread_runtime(runtime_options options = current_thread_defaults())
      : runtime_owner(options) {}
};

/** @brief 由多个工作线程驱动的实验性运行时。 */
class multi_thread_runtime final : public detail::runtime_owner<faio::runtime::mode::MULTI_THREAD> {
 public:
  /** @brief 使用给定选项创建多工作线程运行时。 */
  explicit multi_thread_runtime(runtime_options options = multi_thread_defaults())
      : runtime_owner(options) {}
};

/** @brief 取得当前 faio 执行线程所属的运行时；未绑定时抛出异常。 */
auto this_runtime() -> runtime_ref;
}  // namespace faio::experimental
#endif
