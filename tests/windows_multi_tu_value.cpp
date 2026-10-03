#include "windows_multi_tu_support.hpp"

/** @brief 独立 TU 的外部函数保证 TLS 初始化不能被常量折叠。 */
std::string windows_tls_value(const char* value) {
  return value;
}
