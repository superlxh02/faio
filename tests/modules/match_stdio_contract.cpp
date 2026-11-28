/**
 * @file match_stdio_contract.cpp
 * @brief 在头文件与模块接入下验证模式匹配和标准输出。
 */
#include <expected>
#include <optional>
#include <string>
#include <variant>

#if defined(FAIO_USE_HEADERS) && FAIO_USE_HEADERS
#include <faio/faio.hpp>
#else
import faio;
#endif

namespace {
/** @brief 在常量表达式中检查 optional 分支及 expected<void> 成功分支。 */
constexpr bool match_values() {
  const std::optional<int> value{21};
  const auto doubled = faio::match(
      value, faio::some([](int number) { return number * 2; }), faio::none([] { return 0; }));
  const std::expected<void, int> completed;
  return doubled == 42 && faio::match(completed, faio::ok([] { return true; }), faio::err([](int) {
                                        return false;
                                      }));
}

static_assert(match_values());

/** @brief 用格式化输出验证导入的模板定义及格式化适配器可见。 */
faio::task<bool> print_values() {
  const faio::expected<int> result{42};
  const auto written =
      co_await faio::stdio::println("match={} optional={}", result, std::optional<int>{7});
  if (!written)
    co_return false;
  auto output = faio::stdio::out();
  co_return static_cast<bool>(co_await output.flush());
}
}  // namespace

/** @brief 两种消费方式均要求匹配结果与异步输出成功。 */
int main() {
  const std::variant<int, int> value{std::in_place_index<1>, 42};
  const auto selected = faio::match(value,
                                    faio::index_case<0>([](int) { return 0; }),
                                    faio::index_case<1>([](int number) { return number; }));
  if (selected != 42)
    return 1;
  return faio::block_on(print_values()) ? 0 : 1;
}
