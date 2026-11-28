/**
 * @file faio/detail/common/match.hpp
 * @brief expected、optional、variant 和普通值的有序模式匹配。
 */
#ifndef FAIO_DETAIL_COMMON_MATCH_HPP
#define FAIO_DETAIL_COMMON_MATCH_HPP

#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace faio::detail::match_detail {
enum class subject_kind { VALUE, EXPECTED, OPTIONAL_VALUE, VARIANT };
enum class arm_kind { OK, ERR, SOME, NONE, TYPE, INDEX, VALUE, GUARD, OTHERWISE };

struct no_guard {};

struct no_pattern {};

template <class T>
struct type_pattern {
  using type = T;
};

template <class T>
struct subject_traits {
  static constexpr auto kind = subject_kind::VALUE;
  static constexpr std::size_t branches = 1;
};

template <class T, class E>
struct subject_traits<std::expected<T, E>> {
  static constexpr auto kind = subject_kind::EXPECTED;
  static constexpr std::size_t branches = 2;
};

template <class T>
struct subject_traits<std::optional<T>> {
  static constexpr auto kind = subject_kind::OPTIONAL_VALUE;
  static constexpr std::size_t branches = 2;
};

template <class... T>
struct subject_traits<std::variant<T...>> {
  static constexpr auto kind = subject_kind::VARIANT;
  static constexpr std::size_t branches = sizeof...(T);
};

template <arm_kind Kind,
          class Function,
          class Pattern = no_pattern,
          class Guard = no_guard,
          std::size_t Index = 0>
struct arm {
  static constexpr auto kind = Kind;
  static constexpr auto index = Index;
  static constexpr bool guarded = !std::same_as<Guard, no_guard>;
  using pattern_type = Pattern;

  [[no_unique_address]] Function function;
  [[no_unique_address]] Pattern pattern;
  [[no_unique_address]] Guard guard;
};

template <class T>
inline constexpr bool is_arm = false;

template <arm_kind Kind, class Function, class Pattern, class Guard, std::size_t Index>
inline constexpr bool is_arm<arm<Kind, Function, Pattern, Guard, Index>> = true;

template <subject_kind Kind, std::size_t Branch, class Subject, class Payload = void>
struct context {
  static constexpr auto kind = Kind;
  static constexpr auto branch = Branch;
  using subject_type = Subject;
  using payload_type = Payload;

  Subject&& subject;
  Payload&& payload;
};

template <subject_kind Kind, std::size_t Branch, class Subject>
struct context<Kind, Branch, Subject, void> {
  static constexpr auto kind = Kind;
  static constexpr auto branch = Branch;
  using subject_type = Subject;
  using payload_type = void;

  Subject&& subject;
};

template <std::size_t Branch, class Subject>
constexpr auto make_context(Subject&& subject) {
  constexpr auto kind = subject_traits<std::remove_cvref_t<Subject>>::kind;
  if constexpr (kind == subject_kind::EXPECTED) {
    if constexpr (Branch == 0) {
      if constexpr (std::is_void_v<typename std::remove_cvref_t<Subject>::value_type>)
        return context<kind, Branch, Subject>{std::forward<Subject>(subject)};
      else
        return context<kind, Branch, Subject, decltype(*std::forward<Subject>(subject))>{
            std::forward<Subject>(subject), *std::forward<Subject>(subject)};
    } else {
      return context<kind, Branch, Subject, decltype(std::forward<Subject>(subject).error())>{
          std::forward<Subject>(subject), std::forward<Subject>(subject).error()};
    }
  } else if constexpr (kind == subject_kind::OPTIONAL_VALUE) {
    if constexpr (Branch == 0)
      return context<kind, Branch, Subject, decltype(*std::forward<Subject>(subject))>{
          std::forward<Subject>(subject), *std::forward<Subject>(subject)};
    else
      return context<kind, Branch, Subject>{std::forward<Subject>(subject)};
  } else if constexpr (kind == subject_kind::VARIANT) {
    return context<kind,
                   Branch,
                   Subject,
                   decltype(std::get<Branch>(std::forward<Subject>(subject)))>{
        std::forward<Subject>(subject), std::get<Branch>(std::forward<Subject>(subject))};
  } else {
    return context<kind, Branch, Subject>{std::forward<Subject>(subject)};
  }
}

template <class Context, class T, std::size_t... Indices>
consteval bool applicable_type(std::index_sequence<Indices...>) {
  using variant_type = std::remove_cvref_t<typename Context::subject_type>;
  return (
      (std::same_as<T, std::variant_alternative_t<Indices, variant_type>>
       && std::same_as<typename Context::payload_type,
                       decltype(std::get<Indices>(std::declval<typename Context::subject_type>()))>
       && (Context::branch == std::variant_npos || Context::branch == Indices))
      || ...);
}

template <class T, class Subject, std::size_t... Indices>
constexpr bool holds_type_index(const Subject& subject, std::index_sequence<Indices...>) {
  using variant_type = std::remove_cvref_t<Subject>;
  return ((std::same_as<T, std::variant_alternative_t<Indices, variant_type>>
           && subject.index() == Indices)
          || ...);
}

template <class Context, class Arm>
consteval bool applicable() {
  using pattern = std::remove_cvref_t<Arm>;
  constexpr auto kind = pattern::kind;
  if constexpr (kind == arm_kind::OK || kind == arm_kind::ERR)
    return Context::kind == subject_kind::EXPECTED
           && Context::branch == (kind == arm_kind::OK ? 0 : 1);
  else if constexpr (kind == arm_kind::SOME || kind == arm_kind::NONE)
    return Context::kind == subject_kind::OPTIONAL_VALUE
           && Context::branch == (kind == arm_kind::SOME ? 0 : 1);
  else if constexpr (kind == arm_kind::TYPE) {
    if constexpr (Context::kind != subject_kind::VARIANT)
      return false;
    else
      return match_detail::applicable_type<Context, typename pattern::pattern_type::type>(
          std::make_index_sequence<
              std::variant_size_v<std::remove_cvref_t<typename Context::subject_type>>>{});
  } else if constexpr (kind == arm_kind::INDEX) {
    if constexpr (Context::kind != subject_kind::VARIANT)
      return false;
    else if constexpr (pattern::index
                       >= std::variant_size_v<std::remove_cvref_t<typename Context::subject_type>>)
      return false;
    else
      return (Context::branch == std::variant_npos || Context::branch == pattern::index)
             && std::same_as<typename Context::payload_type,
                             decltype(std::get<pattern::index>(
                                 std::declval<typename Context::subject_type>()))>;
  } else
    return true;
}

template <class Context, class Arm>
inline constexpr bool covers = applicable<Context, Arm>() && !std::remove_cvref_t<Arm>::guarded
                               && (std::remove_cvref_t<Arm>::kind != arm_kind::VALUE
                                   && std::remove_cvref_t<Arm>::kind != arm_kind::GUARD);

template <class Function, class Argument>
constexpr decltype(auto) invoke(Function&& function, Argument&& argument) {
  if constexpr (std::invocable<Function, Argument>)
    return std::invoke(std::forward<Function>(function), std::forward<Argument>(argument));
  else {
    static_assert(std::invocable<Function>,
                  "faio::match handler must accept its matched value or no arguments");
    return std::invoke(std::forward<Function>(function));
  }
}

template <class Context, class Arm>
constexpr decltype(auto) invoke_handler(Context& selected, Arm&& selected_arm) {
  constexpr auto kind = std::remove_cvref_t<Arm>::kind;
  auto&& function = std::forward<Arm>(selected_arm).function;
  if constexpr (kind == arm_kind::OTHERWISE || kind == arm_kind::GUARD || kind == arm_kind::VALUE)
    return match_detail::invoke(std::forward<decltype(function)>(function),
                                std::forward<typename Context::subject_type>(selected.subject));
  else if constexpr (std::is_void_v<typename Context::payload_type>) {
    static_assert(std::invocable<decltype(function)>,
                  "faio::match none/expected<void> handler must accept no arguments");
    return std::invoke(std::forward<decltype(function)>(function));
  } else
    return match_detail::invoke(std::forward<decltype(function)>(function),
                                std::forward<typename Context::payload_type>(selected.payload));
}

template <class Function, class Argument>
constexpr bool invoke_guard(Function&& function, const Argument& argument) {
  static_assert(
      std::is_invocable_r_v<bool, Function, const Argument&>,
      "faio::match guard must accept a const reference and return a bool-convertible value");
  return static_cast<bool>(std::invoke(std::forward<Function>(function), argument));
}

template <class Left, class Right>
constexpr bool values_equal(const Left& left, const Right& right) {
  if constexpr (std::convertible_to<const Left&, std::string_view>
                && std::convertible_to<const Right&, std::string_view>) {
    if constexpr (std::is_pointer_v<Left>)
      if (!left) {
        if constexpr (std::is_pointer_v<Right>)
          return right == nullptr;
        else
          return false;
      }
    if constexpr (std::is_pointer_v<Right>)
      if (!right)
        return false;
    return std::string_view{left} == std::string_view{right};
  } else {
    static_assert(
        requires { static_cast<bool>(left == right); },
        "faio::case_ requires a value comparable with the match subject");
    return static_cast<bool>(left == right);
  }
}

template <class Context, class Arm>
constexpr bool accepts(Context& selected, Arm&& selected_arm) {
  using pattern = std::remove_cvref_t<Arm>;
  constexpr auto kind = pattern::kind;
  if constexpr (kind == arm_kind::INDEX)
    if (selected.subject.index() != pattern::index)
      return false;
  if constexpr (kind == arm_kind::TYPE)
    if (!match_detail::holds_type_index<typename pattern::pattern_type::type>(
            selected.subject,
            std::make_index_sequence<
                std::variant_size_v<std::remove_cvref_t<typename Context::subject_type>>>{}))
      return false;
  if constexpr (kind == arm_kind::VALUE)
    if (!match_detail::values_equal(std::as_const(selected.subject),
                                    std::as_const(selected_arm.pattern)))
      return false;
  if constexpr (kind == arm_kind::GUARD)
    return match_detail::invoke_guard(std::forward<Arm>(selected_arm).pattern, selected.subject);
  else if constexpr (!pattern::guarded)
    return true;
  else if constexpr (std::is_void_v<typename Context::payload_type>) {
    static_assert(std::is_invocable_r_v<bool, decltype((std::forward<Arm>(selected_arm).guard))>,
                  "faio::match guard for a valueless branch must accept no arguments");
    return static_cast<bool>(std::invoke(std::forward<Arm>(selected_arm).guard));
  } else
    return match_detail::invoke_guard(std::forward<Arm>(selected_arm).guard, selected.payload);
}

template <class... T>
struct type_list {};

template <class... Lists>
struct concatenate;

template <>
struct concatenate<> {
  using type = type_list<>;
};

template <class... T>
struct concatenate<type_list<T...>> {
  using type = type_list<T...>;
};

template <class... Left, class... Right, class... Rest>
struct concatenate<type_list<Left...>, type_list<Right...>, Rest...>
    : concatenate<type_list<Left..., Right...>, Rest...> {};

template <class Context, class Arm, bool = applicable<Context, Arm>()>
struct arm_result {
  using type = type_list<>;
};

template <class Context, class Arm>
struct arm_result<Context, Arm, true> {
  using type = type_list<decltype(match_detail::invoke_handler(std::declval<Context&>(),
                                                               std::declval<Arm>()))>;
};

template <class Subject, class Indices, class... Arms>
struct analysis;

template <class Subject, std::size_t... Branches, class... Arms>
struct analysis<Subject, std::index_sequence<Branches...>, Arms...> {
  template <std::size_t Branch>
  using branch_context = decltype(match_detail::make_context<Branch>(std::declval<Subject>()));

  template <std::size_t Branch>
  static constexpr bool covered = (covers<branch_context<Branch>, Arms> || ...);

  template <class Arm>
  static constexpr bool useful = (applicable<branch_context<Branches>, Arm>() || ...);

  template <std::size_t Branch>
  using branch_results =
      typename concatenate<typename arm_result<branch_context<Branch>, Arms>::type...>::type;

  static constexpr bool exhaustive = (covered<Branches> && ...);
  static constexpr bool all_useful = (useful<Arms> && ...);
  using results = typename concatenate<branch_results<Branches>...>::type;
};

template <class List>
struct result_type;

template <>
struct result_type<type_list<>> {
  using type = void;
  static constexpr bool consistent = true;
};

template <class First, class... Rest>
struct result_type<type_list<First, Rest...>> {
  using type = First;
  static constexpr bool consistent = (std::same_as<First, Rest> && ...);
};

template <class Result, std::size_t Index = 0, class Context, class Arms>
constexpr Result dispatch(Context& selected, Arms&& arms) {
  if constexpr (Index == std::tuple_size_v<std::remove_cvref_t<Arms>>)
    std::unreachable();
  else {
    auto&& selected_arm = std::get<Index>(std::forward<Arms>(arms));
    if constexpr (applicable<Context, decltype(selected_arm)>())
      if (match_detail::accepts(selected, std::forward<decltype(selected_arm)>(selected_arm)))
        return match_detail::invoke_handler(selected,
                                            std::forward<decltype(selected_arm)>(selected_arm));
    return match_detail::dispatch<Result, Index + 1>(selected, std::forward<Arms>(arms));
  }
}

template <arm_kind Kind, class Function, class Guard = no_guard>
constexpr auto make_arm(Function&& function, Guard&& guard = {}) {
  return arm<Kind, std::decay_t<Function>, no_pattern, std::decay_t<Guard>>{
      std::forward<Function>(function), {}, std::forward<Guard>(guard)};
}
}  // namespace faio::detail::match_detail

namespace faio {
/** @brief 匹配 expected 的成功值；expected<void> 使用无参处理函数。 */
template <class Function>
[[nodiscard]] constexpr auto ok(Function&& function) {
  return detail::match_detail::make_arm<detail::match_detail::arm_kind::OK>(
      std::forward<Function>(function));
}

/** @brief 守卫观察成功值，返回 true 后才把值交给处理函数。 */
template <class Guard, class Function>
[[nodiscard]] constexpr auto ok(Guard&& guard, Function&& function) {
  return detail::match_detail::make_arm<detail::match_detail::arm_kind::OK>(
      std::forward<Function>(function), std::forward<Guard>(guard));
}

/** @brief 匹配 expected 的错误值。 */
template <class Function>
[[nodiscard]] constexpr auto err(Function&& function) {
  return detail::match_detail::make_arm<detail::match_detail::arm_kind::ERR>(
      std::forward<Function>(function));
}

/** @brief 仅在错误值通过守卫时执行处理函数。 */
template <class Guard, class Function>
[[nodiscard]] constexpr auto err(Guard&& guard, Function&& function) {
  return detail::match_detail::make_arm<detail::match_detail::arm_kind::ERR>(
      std::forward<Function>(function), std::forward<Guard>(guard));
}

/** @brief 匹配 optional 中存在的值。 */
template <class Function>
[[nodiscard]] constexpr auto some(Function&& function) {
  return detail::match_detail::make_arm<detail::match_detail::arm_kind::SOME>(
      std::forward<Function>(function));
}

/** @brief 仅在 optional 的值通过守卫时执行处理函数。 */
template <class Guard, class Function>
[[nodiscard]] constexpr auto some(Guard&& guard, Function&& function) {
  return detail::match_detail::make_arm<detail::match_detail::arm_kind::SOME>(
      std::forward<Function>(function), std::forward<Guard>(guard));
}

/** @brief 匹配空 optional，调用无参处理函数。 */
template <class Function>
[[nodiscard]] constexpr auto none(Function&& function) {
  return detail::match_detail::make_arm<detail::match_detail::arm_kind::NONE>(
      std::forward<Function>(function));
}

/** @brief 按 variant 声明的类型精确匹配，包括该类型的重复 alternative。 */
template <class T, class Function>
[[nodiscard]] constexpr auto type_case(Function&& function) {
  using namespace detail::match_detail;
  return arm<arm_kind::TYPE, std::decay_t<Function>, type_pattern<T>>{
      std::forward<Function>(function), {}, {}};
}

/** @brief 匹配 variant 中指定类型且通过守卫的值。 */
template <class T, class Guard, class Function>
[[nodiscard]] constexpr auto type_case(Guard&& guard, Function&& function) {
  using namespace detail::match_detail;
  return arm<arm_kind::TYPE, std::decay_t<Function>, type_pattern<T>, std::decay_t<Guard>>{
      std::forward<Function>(function), {}, std::forward<Guard>(guard)};
}

/** @brief 按 variant alternative 的位置匹配，可区分重复类型。 */
template <std::size_t Index, class Function>
[[nodiscard]] constexpr auto index_case(Function&& function) {
  using namespace detail::match_detail;
  return arm<arm_kind::INDEX, std::decay_t<Function>, no_pattern, no_guard, Index>{
      std::forward<Function>(function), {}, {}};
}

/** @brief 按 variant 的位置和守卫匹配。 */
template <std::size_t Index, class Guard, class Function>
[[nodiscard]] constexpr auto index_case(Guard&& guard, Function&& function) {
  using namespace detail::match_detail;
  return arm<arm_kind::INDEX, std::decay_t<Function>, no_pattern, std::decay_t<Guard>, Index>{
      std::forward<Function>(function), {}, std::forward<Guard>(guard)};
}

/** @brief 匹配相等的普通值；窄字符串按文本比较，处理函数可接收原值或无参数。 */
template <class Value, class Function>
[[nodiscard]] constexpr auto case_(Value&& value, Function&& function) {
  using namespace detail::match_detail;
  return arm<arm_kind::VALUE, std::decay_t<Function>, std::decay_t<Value>>{
      std::forward<Function>(function), std::forward<Value>(value), {}};
}

/** @brief 守卫以 const 引用观察完整输入，成立后执行处理函数。 */
template <class Guard, class Function>
[[nodiscard]] constexpr auto when(Guard&& guard, Function&& function) {
  using namespace detail::match_detail;
  return arm<arm_kind::GUARD, std::decay_t<Function>, std::decay_t<Guard>>{
      std::forward<Function>(function), std::forward<Guard>(guard), {}};
}

/** @brief 无条件兜底；处理函数可接收完整输入或不接收参数。 */
template <class Function>
[[nodiscard]] constexpr auto otherwise(Function&& function) {
  return detail::match_detail::make_arm<detail::match_detail::arm_kind::OTHERWISE>(
      std::forward<Function>(function));
}

/**
 * @brief 按顺序执行首个匹配分支，保留输入的引用和移动语义。
 * @details expected、optional 和 variant 必须由无守卫分支或 otherwise 穷尽覆盖。
 * 普通值需要 otherwise。所有适用分支的返回类型必须完全一致，包括 void 和引用。
 * 守卫只观察 const 借用；只有选中分支才会把原始值类别传给处理函数。
 * variant 的空异常状态按 std::visit 规则抛出 bad_variant_access。
 */
template <class Subject, class... Arms>
constexpr decltype(auto) match(Subject&& subject, Arms&&... arms) {
  using namespace detail::match_detail;
  static_assert(sizeof...(Arms) != 0, "faio::match requires at least one pattern arm");
  if constexpr (!(is_arm<std::remove_cvref_t<Arms>> && ...)) {
    static_assert(
        (is_arm<std::remove_cvref_t<Arms>> && ...),
        "faio::match expects arms built with ok/err/some/none/type_case/index_case/case_/when/otherwise");
  } else {
    using traits = subject_traits<std::remove_cvref_t<Subject>>;
    using checked = analysis<Subject&&, std::make_index_sequence<traits::branches>, Arms&&...>;
    static_assert(checked::all_useful, "faio::match contains an arm incompatible with its subject");
    static_assert(checked::exhaustive,
                  "faio::match must cover every state with an unguarded arm or otherwise");
    using results = result_type<typename checked::results>;
    static_assert(
        results::consistent,
        "faio::match handlers must return exactly the same type (including references/void)");
    using result = typename results::type;
    auto handlers = std::forward_as_tuple(std::forward<Arms>(arms)...);
    if constexpr (traits::kind == subject_kind::EXPECTED
                  || traits::kind == subject_kind::OPTIONAL_VALUE) {
      if (subject.has_value()) {
        auto selected = detail::match_detail::make_context<0>(std::forward<Subject>(subject));
        return detail::match_detail::dispatch<result>(selected, std::move(handlers));
      }
      auto selected = detail::match_detail::make_context<1>(std::forward<Subject>(subject));
      return detail::match_detail::dispatch<result>(selected, std::move(handlers));
    } else if constexpr (traits::kind == subject_kind::VARIANT) {
      return std::visit(
          [&](auto&& payload) -> result {
            context<subject_kind::VARIANT, std::variant_npos, Subject, decltype(payload)> selected{
                std::forward<Subject>(subject), std::forward<decltype(payload)>(payload)};
            return detail::match_detail::dispatch<result>(selected, std::move(handlers));
          },
          std::forward<Subject>(subject));
    } else {
      auto selected = detail::match_detail::make_context<0>(std::forward<Subject>(subject));
      return detail::match_detail::dispatch<result>(selected, std::move(handlers));
    }
  }
}
}  // namespace faio
#endif
