/**
 * @file faio/detail/experimental/reflection.hpp
 * @brief 实验性的入口注解与发送器接收器适配：运行时选项反射。
 * @details 实验适配在编译期开关下启用，连接阶段建立稳定操作状态；启动、结果交付与关闭沿受控任务图的租约和排空协议推进。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_REFLECTION_HPP
#define FAIO_DETAIL_EXPERIMENTAL_REFLECTION_HPP

#include "faio/detail/experimental/runtime_options.hpp"
#if !FAIO_EXPERIMENTAL_HAS_REFLECTION
#error "faio experimental: reflection capability requires faio::experimental_reflection"
#endif
#include <meta>

namespace faio::experimental {
namespace detail {
consteval runtime_options checked_annotation(runtime_options options) {
  if (const auto error = validate_options(options); error != options_error::none)
    throw options_error_message(error);
  return options;
}
}

template <std::meta::info Entity>
consteval runtime_options runtime_options_of() {
  const auto annotations = std::meta::annotations_of_with_type(Entity, ^^runtime_options);
  if (annotations.size() > 1)
    throw "faio experimental: duplicate runtime_options";
  return detail::checked_annotation(annotations.empty() ? runtime_options{}
      : std::meta::extract<runtime_options>(annotations.front()));
}
}
#endif
