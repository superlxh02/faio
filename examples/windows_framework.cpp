/** @file windows_framework.cpp @brief Windows 完整 IOCP 引擎能力与排空示例。 */
#include <faio/faio.hpp>

int main() {
  auto engine = faio::io::io_engine::create();  // 旧 Windows 工厂入口现在真正初始化 IOCP。
  if (!engine)
    return 1;
  faio::io::engine_config invalid;
  invalid.max_operations = 0;
  auto rejected = faio::io::io_engine::create(invalid);
  if (rejected || rejected.error() != std::errc::invalid_argument)
    return 2;
  auto capabilities = engine->capabilities();
  return std::string_view{capabilities.backend} == "iocp" && capabilities.network
                 && capabilities.filesystem && capabilities.native_filesystem
                 && engine->driver().quiescent()
             ? 0
             : 1;
}
