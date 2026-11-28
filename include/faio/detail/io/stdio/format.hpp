/**
 * @file faio/detail/io/stdio/format.hpp
 * @brief 标准格式化与可选值、结果和资源摘要的适配。
 */
#pragma once

#include <concepts>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace faio::stdio::detail {
/** @brief 只借用当前格式化调用中的参数，不跨协程挂起点保存。 */
template <class T>
struct diagnostic_arg {
  const T* value;
};

/** @brief 保留仅接收可变引用的原生 formatter，不依赖标准库 handle 的 const 策略。 */
template <class T>
struct native_ref_arg {
  T* value;
};

template <class T>
struct optional_traits : std::false_type {};

template <class T>
struct optional_traits<std::optional<T>> : std::true_type {};

template <class T>
struct expected_traits : std::false_type {};

template <class T, class E>
struct expected_traits<std::expected<T, E>> : std::true_type {
  using value_type = T;
};

template <class T>
struct variant_traits : std::false_type {};

template <class... T>
struct variant_traits<std::variant<T...>> : std::true_type {};

namespace tuple_access {
using std::get;

template <std::size_t I, class T>
concept member_get = requires(const T& value) { value.template get<I>(); };

template <std::size_t I, class T>
  requires member_get<I, T> || requires(const T& value) { get<I>(value); }
decltype(auto) element(const T& value) {
  if constexpr (member_get<I, T>)
    return value.template get<I>();
  else
    return get<I>(value);
}
}  // namespace tuple_access

template <class T>
concept tuple_like = requires {
  typename std::tuple_size<T>::type;
} && []<std::size_t... I>(std::index_sequence<I...>) {
  return (requires(const T & value) { tuple_access::element<I>(value); } && ...);
}(std::make_index_sequence<std::tuple_size_v<T>>{});

/** @brief 取得资源摘要使用的静态类型名，不访问对象内部状态。 */
template <class T>
consteval std::string_view diagnostic_type_name() {
#if defined(__clang__)
  constexpr std::string_view text = __PRETTY_FUNCTION__;
  constexpr auto first = text.find("T = ") + 4;
  return text.substr(first, text.rfind(']') - first);
#elif defined(__GNUC__)
  constexpr std::string_view text = __PRETTY_FUNCTION__;
  constexpr auto first = text.find("T = ") + 4;
  constexpr auto last = text.find(';', first);
  return text.substr(first, (last == std::string_view::npos ? text.rfind(']') : last) - first);
#elif defined(_MSC_VER)
  constexpr std::string_view text = __FUNCSIG__;
  constexpr auto first = text.find("diagnostic_type_name<") + sizeof("diagnostic_type_name<") - 1;
  return text.substr(first, text.rfind(">(void)") - first);
#else
  return "object";
#endif
}

/** @brief 摘要省略实现命名空间；类型名称用于诊断，不是稳定的序列化格式。 */
inline void append_type_name(std::string& output, std::string_view name) {
  while (!name.empty()) {
    const auto position = name.find("::detail::");
    if (position == std::string_view::npos) {
      output.append(name);
      return;
    }
    output.append(name.substr(0, position));
    output += "::";
    name.remove_prefix(position + sizeof("::detail::") - 1);
  }
}

template <class T>
void append_diagnostic(std::string& output, const T& value);

/** @brief 格式化元组元素，递归支持 expected 和 optional。 */
template <class T, std::size_t... I>
void append_tuple(std::string& output, const T& value, std::index_sequence<I...>) {
  output += '(';
  std::size_t count{};
  ((output += count++ ? ", " : "",
    faio::stdio::detail::append_diagnostic(output, tuple_access::element<I>(value))),
   ...);
  output += ')';
}

/** @brief 原生 formatter 优先，资源只输出非阻塞摘要，不消费任务或访问异步结果。 */
template <class T>
void append_diagnostic(std::string& output, const T& value) {
  using type = std::remove_cvref_t<T>;
  if constexpr (std::formattable<decltype(value), char>) {
    std::format_to(std::back_inserter(output), "{}", value);
  } else if constexpr (optional_traits<type>::value) {
    if (!value) {
      output += "none";
    } else {
      output += "some(";
      faio::stdio::detail::append_diagnostic(output, *value);
      output += ')';
    }
  } else if constexpr (expected_traits<type>::value) {
    if (value) {
      output += "ok(";
      if constexpr (!std::is_void_v<typename expected_traits<type>::value_type>)
        faio::stdio::detail::append_diagnostic(output, *value);
    } else {
      output += "err(";
      faio::stdio::detail::append_diagnostic(output, value.error());
    }
    output += ')';
  } else if constexpr (variant_traits<type>::value) {
    if (value.valueless_by_exception()) {
      output += "variant(valueless)";
    } else {
      std::format_to(std::back_inserter(output), "variant({}, ", value.index());
      std::visit([&](const auto& item) { faio::stdio::detail::append_diagnostic(output, item); },
                 value);
      output += ')';
    }
  } else if constexpr (std::same_as<type, std::monostate>) {
    output += "()";
  } else if constexpr (std::is_enum_v<type>) {
    faio::stdio::detail::append_diagnostic(output, std::to_underlying(value));
  } else if constexpr (requires {
                         { value.to_string() } -> std::convertible_to<std::string_view>;
                       }) {
    output += value.to_string();
  } else if constexpr (tuple_like<type>) {
    faio::stdio::detail::append_tuple(
        output, value, std::make_index_sequence<std::tuple_size_v<type>>{});
  } else if constexpr (std::ranges::forward_range<const type>) {
    output += '[';
    bool first = true;
    for (const auto& item : value) {
      if (!std::exchange(first, false))
        output += ", ";
      faio::stdio::detail::append_diagnostic(output, item);
    }
    output += ']';
  } else {
    output += '<';
    append_type_name(output, diagnostic_type_name<type>());
    if constexpr (requires {
                    { value.fd() } noexcept -> std::integral;
                  }) {
      std::format_to(std::back_inserter(output), " fd={}", value.fd());
    } else if constexpr (requires {
                           { value.size() } noexcept -> std::integral;
                         }) {
      std::format_to(std::back_inserter(output), " size={}", value.size());
    } else if constexpr (requires {
                           { value.done() } noexcept -> std::convertible_to<bool>;
                         }) {
      std::format_to(std::back_inserter(output), " done={}", value.done());
    } else if constexpr (requires {
                           { value.valid() } noexcept -> std::convertible_to<bool>;
                         }) {
      std::format_to(std::back_inserter(output), " valid={}", value.valid());
    }
    output += '>';
  }
}

/** @brief 原生可格式化参数保持原类型和格式说明符。 */
template <class T>
inline constexpr bool native_format_arg = std::formattable<T, char>;

template <class T>
inline constexpr bool mutable_native_format_arg =
    native_format_arg<T> && !std::formattable<const std::remove_reference_t<T>, char>;

template <class T>
using format_arg_type = std::conditional_t<
    mutable_native_format_arg<T>,
    native_ref_arg<std::remove_reference_t<T>>,
    std::conditional_t<native_format_arg<T>, T, diagnostic_arg<std::remove_reference_t<T>>>>;

template <class T>
decltype(auto) adapt_format_arg(T&& value) noexcept {
  if constexpr (mutable_native_format_arg<T>)
    return native_ref_arg<std::remove_reference_t<T>>{std::addressof(value)};
  else if constexpr (native_format_arg<T>)
    return std::forward<T>(value);
  else
    return diagnostic_arg<std::remove_reference_t<T>>{std::addressof(value)};
}
}  // namespace faio::stdio::detail

namespace std {
/** @brief 格式说明沿用原 formatter，透过指针保留实参的可变引用。 */
template <class T>
struct formatter<faio::stdio::detail::native_ref_arg<T>, char> {
  formatter<remove_cvref_t<T>, char> native;

  constexpr auto parse(format_parse_context& context) { return native.parse(context); }

  template <class Context>
  auto format(const faio::stdio::detail::native_ref_arg<T>& argument, Context& context) const {
    return native.format(*argument.value, context);
  }
};

/** @brief 只为 faio 自有适配类型提供 formatter，不特化标准 expected/optional。 */
template <class T>
struct formatter<faio::stdio::detail::diagnostic_arg<T>, char> : formatter<string_view, char> {
  template <class Context>
  auto format(const faio::stdio::detail::diagnostic_arg<T>& argument, Context& context) const {
    string text;
    faio::stdio::detail::append_diagnostic(text, *argument.value);
    return formatter<string_view, char>::format(text, context);
  }
};
}  // namespace std

namespace faio::stdio {
/** @brief 与 print 共用的编译期格式串，非原生类型使用文本摘要格式说明符。 */
template <class... Args>
using format_string = std::format_string<detail::format_arg_type<Args>...>;

/** @brief 同步格式化并拥有结果；不消费 expected/optional，也不等待任务或资源。 */
template <class... Args>
[[nodiscard]] std::string format(format_string<Args...> text, Args&&... args) {
  return std::format(text, detail::adapt_format_arg(std::forward<Args>(args))...);
}
}  // namespace faio::stdio
