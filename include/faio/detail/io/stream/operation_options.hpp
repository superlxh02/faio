/**
 * @file operation_options.hpp
 * @brief 异步协议和进程操作共用的截止时间与附加停止令牌。
 */
#pragma once
#include <chrono>
#include <optional>
#include <stop_token>

namespace faio::io {
/**
 * @brief 一次操作的绝对截止时间与附加取消源；任一取消源触发即请求停止。
 * @details stop 与当前协程继承的停止令牌共同生效。deadline 采用单调时钟，
 * 无值表示本选项不添加时间限制；接口仍可施加自身的默认超时。
 */
struct operation_options {
  std::optional<std::chrono::steady_clock::time_point>
      deadline{};         /**< 操作必须在此单调时钟时刻前提交结果。 */
  std::stop_token stop{}; /**< 调用者额外提供的取消源，不替换父协程令牌。 */
};
}  // namespace faio::io
