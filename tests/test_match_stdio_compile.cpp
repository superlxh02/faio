/**
 * @file test_match_stdio_compile.cpp
 * @brief 验证模式覆盖、分支结果与格式串的编译期检查。
 */
#include "faio/common/match.hpp"
#include <expected>
#include <optional>
#include <string>
#include <variant>

#if FAIO_MATCH_STDIO_CASE == 0 || FAIO_MATCH_STDIO_CASE >= 6
#include "faio/stdio/print.hpp"
#endif

/** @brief 每个独立目标仅实例化一个成功或失败的公共接口用例。 */
void check_match_stdio() {
#if FAIO_MATCH_STDIO_CASE == 0
  constexpr std::expected<int, int> result{42};
  static_assert(faio::match(result, faio::ok([](int value) { return value; }), faio::err([](int) {
                              return 0;
                            }))
                == 42);
  constexpr std::optional<int> optional{7};
  static_assert(faio::match(optional, faio::some([](int value) { return value; }), faio::none([] {
                              return 0;
                            }))
                == 7);
  constexpr std::variant<int, int> indexed{std::in_place_index<1>, 42};
  static_assert(faio::match(indexed,
                            faio::index_case<0>([](int) { return 0; }),
                            faio::index_case<1>([](int value) { return value; }))
                == 42);
  (void)faio::stdio::print("value={:04d} optional={}", 42, optional);
  (void)faio::stdio::format("{}", result);
#elif FAIO_MATCH_STDIO_CASE == 1
  std::expected<int, int> result{42};
  (void)faio::match(result, faio::ok([](int value) { return value; }));
#elif FAIO_MATCH_STDIO_CASE == 2
  std::optional<int> value{42};
  (void)faio::match(
      value,
      faio::some([](int number) { return number > 0; }, [](int number) { return number; }),
      faio::none([] { return 0; }));
#elif FAIO_MATCH_STDIO_CASE == 3
  (void)faio::match(
      42, faio::when([](int number) { return number > 0; }, [](int number) { return number; }));
#elif FAIO_MATCH_STDIO_CASE == 4
  std::variant<int, std::string> value{42};
  (void)faio::match(value, faio::type_case<int>([](int number) { return number; }));
#elif FAIO_MATCH_STDIO_CASE == 5
  std::expected<int, int> result{42};
  (void)faio::match(result, faio::ok([](int) { return 42; }), faio::err([](int) { return 0.0; }));
#elif FAIO_MATCH_STDIO_CASE == 6
  (void)faio::stdio::print("{:d}", std::string{"text"});
#elif FAIO_MATCH_STDIO_CASE == 7
  (void)faio::stdio::print("{} {}", 42);
#else
#error Unknown match/stdio compile test case
#endif
}
