/**
 * @file common/match.cpp
 * @brief 匹配 expected、optional 和普通值，并返回统一的结果类型。
 */
#include <faio/common/error.hpp>
#include <faio/common/match.hpp>

#include <iostream>
#include <optional>

/** @brief 检查成功、空值与带条件的分支，并打印选择结果。 */
int main() {
  const faio::expected<int> answer{42};
  const int value = faio::match(answer,
                                faio::ok([](int number) { return number; }),
                                faio::err([](const faio::error&) { return 0; }));

  const std::optional<int> absent;
  const int fallback = faio::match(
      absent, faio::some([](int number) { return number; }), faio::none([] { return 7; }));

  const char* category =
      faio::match(value,
                  faio::case_(0, [] { return "zero"; }),
                  faio::when([](int number) { return number > 0; }, [] { return "positive"; }),
                  faio::otherwise([] { return "negative"; }));
  if (value != 42 || fallback != 7)
    return 1;

  std::cout << "value=" << value << ", fallback=" << fallback << ", category=" << category << '\n';
  return 0;
}
