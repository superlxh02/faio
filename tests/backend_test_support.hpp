/**
 * @file backend_test_support.hpp
 * @brief 测试进程显式选择 Linux IO 后端，生产配置不读取测试环境变量。
 * @details 同一套行为测试分别由 FAIO_TEST_IO_BACKEND=uring/epoll 执行；
 *          未设置变量时保留库自身的默认后端选择规则。
 */
#pragma once
#include "faio/faio.hpp"
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace faio_test {
#if defined(__linux__)
/** @brief 读取矩阵配置；空值保持默认选择，非法值在创建服务线程之前失败。 */
inline std::optional<faio::runtime::io_backend> requested_backend() {
  const char *raw = std::getenv("FAIO_TEST_IO_BACKEND");
  if (!raw || !*raw)
    return std::nullopt;
  const std::string_view requested{raw};
  if (requested == "epoll")
    return faio::runtime::io_backend::IO_EPOLL;
  if (requested == "uring")
    return faio::runtime::io_backend::IO_URING;
  throw std::invalid_argument(
      "FAIO_TEST_IO_BACKEND must be epoll or uring on Linux");
}
#endif
/** @brief 所有测试的 runtime 配置入口；非法或不适用的后端立即失败。 */
inline faio::ConfigBuilder config_builder() {
  faio::ConfigBuilder builder;
  const char *raw = std::getenv("FAIO_TEST_IO_BACKEND");
  if (!raw || !*raw)
    return builder;
  const std::string_view requested{raw};
#if defined(__linux__)
  if (requested == "epoll")
    return builder.set_io_backend(faio::runtime::io_backend::IO_EPOLL);
  if (requested == "uring")
    return builder.set_io_backend(faio::runtime::io_backend::IO_URING);
#endif
  throw std::invalid_argument(
      "FAIO_TEST_IO_BACKEND must name a backend supported by this platform");
}
/** @brief 独立 engine 契约和 runtime 使用相同的后端矩阵配置。 */
inline faio::io::engine_config engine_config() {
  faio::io::engine_config config;
#if defined(__linux__)
  config.requested_backend = requested_backend();
#endif
  return config;
}
/** @brief 阻塞 provider 队列契约明确选择 reactor；原生文件路径另行验证。 */
inline faio::io::engine_config provider_engine_config() {
  auto config = engine_config();
#if defined(__linux__)
  config.requested_backend = faio::runtime::io_backend::IO_EPOLL;
#endif
  return config;
}
} // namespace faio_test
