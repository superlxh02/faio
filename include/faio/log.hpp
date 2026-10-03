#ifndef FAIO_LOG_HPP
#define FAIO_LOG_HPP

#include <memory>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <utility>

namespace faio::log {

// 独立的线程安全 logger，不修改应用的 spdlog 默认 logger 或全局配置。
// 默认输出到 stderr；返回引用避免每条日志复制 shared_ptr。
inline auto logger() -> const std::shared_ptr<spdlog::logger> & {
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

} // namespace faio::log

#endif // FAIO_LOG_HPP
