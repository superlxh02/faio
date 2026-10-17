/**
 * @file faio/detail/io/traits/backend.hpp
 * @brief 跨平台异步 IO：后端封装与提交协议。
 * @details 以编译期约束检查接口形状及返回类型；概念检查本身不执行 IO，也不承担资源生命周期。
 */
#pragma once
#include "faio/detail/io/protocol/backend.hpp"
#include <concepts>
#include <optional>
#include <span>

namespace faio::io::detail {
/** @brief 具体后端的最小合同；构造时检查，允许平台别名先声明不完整类型。 */
template <class B>
concept backend_implementation = requires(B& object,
                                          backend_operation op,
                                          std::span<backend_event> events,
                                          std::optional<int> timeout,
                                          std::uint32_t kind) {
  { B::native_proactor } -> std::convertible_to<bool>;
  { object.try_submit(op) } noexcept -> std::same_as<backend_submit_result>;
  { object.flush() } noexcept -> std::same_as<backend_flush_result>;
  { object.poll(events, timeout) } noexcept -> std::same_as<int>;
  { object.wake() } noexcept;
  { object.begin_shutdown() } noexcept;
  { object.quiescent() } noexcept -> std::same_as<bool>;
  { object.supports(kind) } noexcept -> std::same_as<bool>;
  { object.name() } noexcept -> std::convertible_to<const char*>;
  { object.statistics() } noexcept -> std::same_as<backend_statistics>;
};

}
