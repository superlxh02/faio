/**
 * @file faio/detail/experimental/lifecycle.hpp
 * @brief 构造协程的 promise 时读取函数注解，自动选择生命周期策略。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_LIFECYCLE_HPP
#define FAIO_DETAIL_EXPERIMENTAL_LIFECYCLE_HPP

#include "faio/detail/coroutine/lifecycle/owned_hooks.hpp"
#include "faio/detail/experimental/reflection.hpp"
#include <concepts>
#include <cstddef>
#include <meta>

#if !defined(FAIO_EXPERIMENTAL_TASK_LIFECYCLE) || !FAIO_EXPERIMENTAL_TASK_LIFECYCLE
#error "faio experimental: lifecycle requires the experimental_lifecycle component"
#endif

namespace faio::experimental {
/**
 * @brief 为协程函数添加注解；调用函数、co_await 和 spawn 时，策略均通过该协程自身的 promise 生效。
 * @details 按类型选择策略，并为每个任务默认构造策略对象，不捕获函数的运行期参数。返回 task 的
 * 普通转发函数不创建自己的 promise，因此其注解不会生效。所有使用该组件的 task 翻译单元
 * 必须统一启用 C++26 和 FAIO_EXPERIMENTAL_TASK_LIFECYCLE，不能与普通模式混合编译。
 */
template <class Policy>
struct lifecycle {
  static_assert(task_lifecycle_policy<Policy> && std::default_initializable<Policy>,
                "invalid lifecycle policy: requires default construction, void before(), and "
                "noexcept void after(const faio::task_exit&)");
  using policy_type = Policy;
};

namespace detail {
/** @brief 元数据只在常量求值期间存在；运行期核心只接收类型固定的普通描述值。 */
struct lifecycle_annotation_selection {
  std::meta::info policy{};
  std::size_t count{};
};

/** @brief 选择当前协程的策略标签，其他注解不参与生命周期处理。 */
consteval lifecycle_annotation_selection select_lifecycle_annotation(std::meta::info entity) {
  lifecycle_annotation_selection selection;
  if (!std::meta::is_function(entity))
    return selection;
  for (const auto annotation : std::meta::annotations_of(entity)) {
    const auto type = std::meta::type_of(annotation);
    if (std::meta::has_template_arguments(type) && std::meta::template_of(type) == ^^lifecycle) {
      selection.policy = std::meta::template_arguments_of(type).front();
      ++selection.count;
    }
  }
  return selection;
}

/**
 * @brief 在实际 promise 构造函数的默认函数实参中调用，以取得原协程函数实体。
 * @details current_function 必须作为普通值实参传入；若作为模板实参，会在声明处
 * 求值，无法再识别原函数。substitute 生成具体工厂，extract 只输出签名固定的函数指针。
 */
consteval faio::detail::task_lifecycle_configuration task_lifecycle_hooks_for(
    std::meta::info entity) {
  const auto selection = select_lifecycle_annotation(entity);
  if (selection.count == 0)
    return {};
  if (selection.count != 1)
    throw "duplicate lifecycle annotation";
  const auto factory = std::meta::substitute(^^faio::detail::make_default_task_lifecycle,
                                             {
                                                 selection.policy});
  return {std::meta::extract<faio::detail::task_lifecycle_factory>(factory)};
}
}  // namespace detail
}  // namespace faio::experimental
#endif
