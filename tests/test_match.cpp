/**
 * @file test_match.cpp
 * @brief 有序模式、穷尽分支、引用转发及异常传播的单元测试。
 */
#include <faio/common/error.hpp>
#include <faio/common/match.hpp>
#include <gtest/gtest.h>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {
constexpr int constant_match() {
  const std::expected<int, int> result{std::unexpected{7}};
  const auto first = faio::match(result,
                                 faio::ok([](int value) { return value; }),
                                 faio::err([](int value) { return -value; }));
  const auto second = faio::match(std::optional<int>{3},
                                  faio::some([](int value) { return value * 2; }),
                                  faio::none([] { return 0; }));
  const auto third =
      faio::match(5,
                  faio::case_(4, [] { return 40; }),
                  faio::when([](int value) { return value > 4; }, [](int value) { return value; }),
                  faio::otherwise([] { return 0; }));
  return first + second + third;
}

static_assert(constant_match() == 4);
static_assert(faio::match(std::variant<int, char>{'x'},
                          faio::type_case<int>([](int n) { return n; }),
                          faio::type_case<char>([](char n) { return static_cast<int>(n); }))
              == 'x');

struct lvalue_guard {
  bool operator()() & { return true; }
};

struct rvalue_handler {
  std::unique_ptr<int> owned;

  int operator()(int value) && { return value + *owned; }
};

struct failing_alternative {
  failing_alternative() { throw std::runtime_error("construction"); }

  failing_alternative(failing_alternative&&) { throw std::runtime_error("move"); }
};

enum class color { RED, GREEN, BLUE };
}  // namespace

namespace match_adl_fixture {
struct value {
  int number;

  friend constexpr bool operator==(const value&, const value&) = default;

  friend bool values_equal(const value&, const value&) { return false; }
};
}  // namespace match_adl_fixture

TEST(Match, ExpectedDistinguishesIdenticalValueAndErrorTypes) {
  std::expected<int, int> value{11};
  std::expected<int, int> error{std::unexpected{13}};
  auto success = faio::ok([](int value) { return value * 2; });
  auto failure = faio::err([](int value) { return -value; });
  EXPECT_EQ(faio::match(value, success, failure), 22);
  EXPECT_EQ(faio::match(error, success, failure), -13);
}

TEST(Match, LibraryExpectedUsesTheSameArms) {
  faio::expected<int> value{42};
  faio::expected<int> failure{std::unexpected{faio::make_error(EINVAL)}};
  auto success = faio::ok([](int number) { return number; });
  auto error = faio::err([](const faio::error& error) { return -error.value(); });
  EXPECT_EQ(faio::match(value, success, error), 42);
  EXPECT_EQ(faio::match(failure, success, error), -EINVAL);
}

TEST(Match, ExpectedVoidCallsTheSuccessHandlerWithoutAValue) {
  std::expected<void, int> value;
  std::expected<void, int> failure{std::unexpected{9}};
  EXPECT_EQ(faio::match(value, faio::ok([] { return 1; }), faio::err([](int n) { return -n; })), 1);
  EXPECT_EQ(faio::match(failure, faio::ok([] { return 1; }), faio::err([](int n) { return -n; })),
            -9);

  auto guarded = faio::ok(lvalue_guard{}, [] { return 5; });
  EXPECT_EQ(faio::match(value, guarded, faio::ok([] { return 0; }), faio::err([] { return -1; })),
            5);
}

TEST(Match, OptionalCoversSomeAndNone) {
  std::optional<std::string> value{"present"};
  std::optional<std::string> empty;
  auto some = faio::some([](const std::string& text) { return text.size(); });
  auto none = faio::none([] { return std::size_t{0}; });
  EXPECT_EQ(faio::match(value, some, none), 7u);
  EXPECT_EQ(faio::match(empty, some, none), 0u);
}

TEST(Match, PreservesMutableAndConstReferences) {
  std::optional<int> value{4};
  int fallback = 9;
  auto& selected = faio::match(value,
                               faio::some([](int& number) -> int& { return number; }),
                               faio::none([&]() -> int& { return fallback; }));
  static_assert(std::same_as<decltype(selected), int&>);
  selected = 17;
  EXPECT_EQ(*value, 17);

  const std::optional<int>& immutable = value;
  auto immutable_some = faio::some([](const int& number) -> const int& { return number; });
  auto immutable_none = faio::none([&]() -> const int& { return fallback; });
  const auto& borrowed = faio::match(immutable, immutable_some, immutable_none);
  EXPECT_EQ(&borrowed, &*value);
}

TEST(Match, MovesOnlyTheSelectedExpectedPayloadAfterGuards) {
  std::expected<std::unique_ptr<int>, int> value{std::make_unique<int>(29)};
  int guards = 0;
  bool wrong_handler = false;
  const auto result = faio::match(std::move(value),
                                  faio::ok(
                                      [&](const std::unique_ptr<int>& owned) {
                                        ++guards;
                                        EXPECT_EQ(*owned, 29);
                                        return false;
                                      },
                                      [&](std::unique_ptr<int>) {
                                        wrong_handler = true;
                                        return 0;
                                      }),
                                  faio::ok(
                                      [&](const std::unique_ptr<int>& owned) {
                                        ++guards;
                                        EXPECT_EQ(*owned, 29);
                                        return true;
                                      },
                                      [](std::unique_ptr<int> owned) { return *owned; }),
                                  faio::ok([] { return -2; }),
                                  faio::err([] { return -1; }));
  EXPECT_EQ(result, 29);
  EXPECT_EQ(guards, 2);
  EXPECT_FALSE(wrong_handler);
  EXPECT_FALSE(*value);

  std::expected<int, std::unique_ptr<int>> failure{std::unexpected{std::make_unique<int>(23)}};
  EXPECT_EQ(faio::match(std::move(failure),
                        faio::ok([](int n) { return n; }),
                        faio::err([](std::unique_ptr<int> owned) { return -*owned; })),
            -23);
  EXPECT_FALSE(failure.error());
}

TEST(Match, CanReturnMoveOnlyValues) {
  std::optional<std::unique_ptr<int>> value{std::make_unique<int>(31)};
  auto result = faio::match(std::move(value),
                            faio::some([](std::unique_ptr<int> owned) { return owned; }),
                            faio::none([] { return std::unique_ptr<int>{}; }));
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 31);
  EXPECT_FALSE(*value);
}

TEST(Match, SupportsMoveOnlyAndRefQualifiedHandlers) {
  EXPECT_EQ(faio::match(std::optional<int>{3},
                        faio::some(rvalue_handler{std::make_unique<int>(7)}),
                        faio::none([] { return 0; })),
            10);
  auto some = faio::some([count = 0](int value) mutable { return value + ++count; });
  auto none = faio::none([] { return 0; });
  EXPECT_EQ(faio::match(std::optional<int>{3}, some, none), 4);
  EXPECT_EQ(faio::match(std::optional<int>{3}, some, none), 5);
}

TEST(Match, VariantSupportsTypedArmsAndTypeGuards) {
  std::variant<int, std::string> value{std::string{"hello"}};
  int calls = 0;
  EXPECT_EQ(faio::match(value,
                        faio::type_case<int>([](int n) { return n; }),
                        faio::type_case<std::string>(
                            [&](const std::string& text) {
                              ++calls;
                              return text.size() > 10;
                            },
                            [](const std::string&) { return 100; }),
                        faio::type_case<std::string>(
                            [](const std::string& text) { return static_cast<int>(text.size()); })),
            5);
  EXPECT_EQ(calls, 1);
}

TEST(Match, VariantIndexArmsDistinguishRepeatedTypes) {
  std::variant<int, int> value{std::in_place_index<1>, 7};
  EXPECT_EQ(faio::match(value,
                        faio::index_case<0>([](int n) { return n; }),
                        faio::index_case<1>([](int) { return false; }, [](int) { return 100; }),
                        faio::index_case<1>([](int n) { return n * 2; })),
            14);
  EXPECT_EQ(faio::match(value, faio::type_case<int>([](int n) { return n + 1; })), 8);

  const std::variant<const int, std::string> immutable{std::in_place_index<0>, 19};
  EXPECT_EQ(faio::match(immutable,
                        faio::index_case<0>([](const int& n) { return n; }),
                        faio::index_case<1>([] { return 0; })),
            19);
}

TEST(Match, VariantTypeArmsDistinguishDeclaredConstAlternatives) {
  std::variant<int, const int> value{std::in_place_index<0>, 11};
  auto plain = faio::type_case<int>([](int& n) { return ++n; });
  auto immutable = faio::type_case<const int>([](const int& n) { return n * 2; });
  EXPECT_EQ(faio::match(value, plain, immutable), 12);
  EXPECT_EQ(std::get<0>(value), 12);
  value.emplace<1>(13);
  EXPECT_EQ(faio::match(value, plain, immutable), 26);

  auto by_index_plain = faio::index_case<0>([](int& n) { return ++n; });
  auto by_index_const = faio::index_case<1>([](const int& n) { return n * 3; });
  EXPECT_EQ(faio::match(value, by_index_plain, by_index_const), 39);

  const auto& borrowed = value;
  auto inspect_plain = faio::type_case<int>([](const int& n) { return n + 1; });
  auto inspect_const = faio::type_case<const int>([](const int& n) { return n + 2; });
  EXPECT_EQ(faio::match(borrowed, inspect_plain, inspect_const), 15);
  value.emplace<0>(17);
  EXPECT_EQ(faio::match(borrowed, inspect_plain, inspect_const), 18);
  EXPECT_EQ(faio::match(value, by_index_plain, by_index_const), 18);
}

TEST(Match, VariantMovesItsAlternativeAndPreservesReferences) {
  std::variant<std::unique_ptr<int>, int> value{std::make_unique<int>(37)};
  EXPECT_EQ(faio::match(std::move(value),
                        faio::type_case<std::unique_ptr<int>>(
                            [](std::unique_ptr<int> owned) { return *owned; }),
                        faio::type_case<int>([](int n) { return n; })),
            37);
  EXPECT_FALSE(std::get<0>(value));

  std::variant<int, int> repeated{std::in_place_index<0>, 6};
  auto& selected = faio::match(repeated, faio::type_case<int>([](int& n) -> int& { return n; }));
  selected = 8;
  EXPECT_EQ(std::get<0>(repeated), 8);
}

TEST(Match, ValuelessVariantPreservesVisitExceptionSemantics) {
  std::variant<int, failing_alternative> value{7};
  EXPECT_THROW(value.emplace<1>(), std::runtime_error);
  ASSERT_TRUE(value.valueless_by_exception());
  EXPECT_THROW((void)faio::match(value,
                                 faio::type_case<int>([](int n) { return n; }),
                                 faio::type_case<failing_alternative>([] { return 0; })),
               std::bad_variant_access);
}

TEST(Match, GuardsAndFallbackObserveTheWholeSubject) {
  std::expected<int, std::string> failure{std::unexpected{"failed"}};
  EXPECT_EQ(faio::match(
                failure,
                faio::ok([](int n) { return n; }),
                faio::when([](const auto& result) { return !result && result.error() == "failed"; },
                           [](const auto&) { return 8; }),
                faio::otherwise([] { return -1; })),
            8);
  std::optional<int> empty;
  EXPECT_EQ(
      faio::match(empty, faio::otherwise([](const auto& value) { return value.has_value(); })),
      false);
}

TEST(Match, ScalarAndEnumPatternsAreOrdered) {
  int late_guards = 0;
  EXPECT_EQ(faio::match(3,
                        faio::case_(3, [] { return 7; }),
                        faio::when(
                            [&](int) {
                              ++late_guards;
                              return true;
                            },
                            [] { return 8; }),
                        faio::otherwise([] { return 9; })),
            7);
  EXPECT_EQ(late_guards, 0);
  EXPECT_EQ(faio::match(color::GREEN,
                        faio::case_(color::RED, [] { return 1; }),
                        faio::case_(color::GREEN, [](color) { return 2; }),
                        faio::otherwise([] { return 3; })),
            2);
}

TEST(Match, InternalHelpersDoNotParticipateInSubjectAdl) {
  EXPECT_EQ(faio::match(match_adl_fixture::value{5},
                        faio::case_(match_adl_fixture::value{5}, [] { return 1; }),
                        faio::otherwise([] { return 0; })),
            1);
}

TEST(Match, StringPatternsCompareTextAndHandleNullPointers) {
  const std::string value{"alpha"};
  EXPECT_EQ(
      faio::match(value, faio::case_("alpha", [] { return 1; }), faio::otherwise([] { return 0; })),
      1);
  char stored[]{"alpha"};
  EXPECT_EQ(faio::match(
                stored, faio::case_("alpha", [] { return 2; }), faio::otherwise([] { return 0; })),
            2);
  const char* empty = nullptr;
  EXPECT_EQ(faio::match(empty,
                        faio::case_("alpha", [] { return 1; }),
                        faio::case_(nullptr, [] { return 2; }),
                        faio::otherwise([] { return 0; })),
            2);
}

TEST(Match, VoidHandlersAndNestedMatching) {
  int calls = 0;
  faio::match(std::optional<int>{4}, faio::some([&](int n) { calls += n; }), faio::none([&] {
                calls = -1;
              }));
  EXPECT_EQ(calls, 4);
  std::expected<std::optional<int>, int> nested{std::optional<int>{6}};
  EXPECT_EQ(faio::match(nested,
                        faio::ok([](const auto& value) {
                          return faio::match(value,
                                             faio::some([](int n) { return n; }),
                                             faio::none([] { return 0; }));
                        }),
                        faio::err([](int n) { return -n; })),
            6);
}

TEST(Match, ExceptionsFromGuardsAndHandlersPropagate) {
  std::optional<int> value{1};
  EXPECT_THROW((void)faio::match(value,
                                 faio::some([](int) -> bool { throw std::runtime_error("guard"); },
                                            [](int n) { return n; }),
                                 faio::some([](int n) { return n; }),
                                 faio::none([] { return 0; })),
               std::runtime_error);
  EXPECT_THROW(
      (void)faio::match(value,
                        faio::some([](int) -> int { throw std::runtime_error("handler"); }),
                        faio::none([] { return 0; })),
      std::runtime_error);
}
