/** @file windows_framework.cpp @brief
 * Windows框架能力检查；未实现的后端明确拒绝创建。 */
#include <faio/faio.hpp>
int main() {
  auto engine = faio::io::io_engine::create();
  return !engine && engine.error() == faio::io::windows::unavailable_error()
             ? 0
             : 1;
}
