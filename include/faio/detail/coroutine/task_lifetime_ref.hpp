#ifndef FAIO_DETAIL_COROUTINE_TASK_LIFETIME_REF_HPP
#define FAIO_DETAIL_COROUTINE_TASK_LIFETIME_REF_HPP

#include <concepts>
#include <memory>
#include <type_traits>

namespace faio {
// 根任务生命周期协议，与只负责入队的 coroutine_scheduler 分开。
// 登记和归还必须成对；所有引用都只借用生命周期服务。
template <class L>
concept task_lifetime = requires(L& lifetime) {
  { lifetime.register_task() } noexcept -> std::same_as<void>;
  { lifetime.finish_task() } noexcept -> std::same_as<void>;
};

// 可选的根任务计数服务；无运行时生命周期服务的自定义调度器可以使用空引用。
class task_lifetime_ref {
 public:
  // 空引用表示提交方自行负责调度器存活，登记与归还均为空操作。
  task_lifetime_ref() noexcept = default;

  // 借用具体计数服务并生成与真实类型匹配的操作表，无额外分配。
  template <task_lifetime L>
    requires(!std::same_as<std::remove_cvref_t<L>, task_lifetime_ref>)
  explicit task_lifetime_ref(L& lifetime) noexcept
      : state_(std::addressof(lifetime)), ops_(&operations_for<L>) {}

  // 查询是否绑定了计数服务。
  explicit operator bool() const noexcept { return state_ != nullptr; }

  // 在根任务提交前登记；空服务不增加任何计数。
  void register_task() const noexcept { ops_->register_task(state_); }

  // 根帧析构时归还登记；必须只归还已经成功登记的任务。
  void finish_task() const noexcept { ops_->finish_task(state_); }

 private:
  // 生命周期服务的静态操作表，不与 scheduler_ref 的入队表混合。
  struct operations {
    void (*register_task)(void*) noexcept;  // 增加一个活动根任务。
    void (*finish_task)(void*) noexcept;    // 归还一个活动根任务。
  };

  // 所有同类型引用共用此表，类型转换仅由绑定构造函数决定。
  template <task_lifetime L>
  static inline constexpr operations operations_for{
      +[](void* state) noexcept { static_cast<L*>(state)->register_task(); },
      +[](void* state) noexcept { static_cast<L*>(state)->finish_task(); }};
  // 空服务共用无操作表，运行时绑定服务的热路径无需额外判空分支。
  static inline constexpr operations empty_operations{+[](void*) noexcept {},
                                                      +[](void*) noexcept {}};
  void* state_{};                             // 借用具体生命周期服务，不拥有对象。
  const operations* ops_{&empty_operations};  // 借用类型匹配的静态操作表，空服务也有效。
};
}  // namespace faio
#endif  // FAIO_DETAIL_COROUTINE_TASK_LIFETIME_REF_HPP
