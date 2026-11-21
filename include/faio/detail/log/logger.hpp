/**
 * @file faio/detail/log/logger.hpp
 * @brief 日志接口入口。
 */
#ifndef FAIO_DETAIL_LOG_LOGGER_HPP
#define FAIO_DETAIL_LOG_LOGGER_HPP

#include <memory>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <utility>

namespace faio::log {
/**
 * @brief 独立的线程安全 logger，保留应用的 spdlog 默认 logger 和全局配置。
 * 默认输出到 stderr；返回引用，避免每次记录日志都复制 shared_ptr。
 */
inline auto logger() -> const std::shared_ptr<spdlog::logger>& {
  static const auto instance = [] {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto result = std::make_shared<spdlog::logger>("faio", std::move(sink));
    result->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] [thread %t] %v");
    result->set_level(spdlog::level::info);
    result->flush_on(spdlog::level::warn);
    return result;
  }();
  return instance;
}
}  // namespace faio::log
#endif
