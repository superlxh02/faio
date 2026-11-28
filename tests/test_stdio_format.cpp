/**
 * @file test_stdio_format.cpp
 * @brief 格式化适配、参数所有权与通用异步流输出。
 */
#include <faio/faio.hpp>
#include <gtest/gtest.h>
#include <array>
#include <cerrno>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace stdio_format_test {
struct native_value {
  int value;
};

struct throwing_value {};

struct mutable_value {
  int number;
};

void append_diagnostic(std::string&, const mutable_value&) = delete;

struct adl_tuple {
  int first;
  std::optional<int> second;
};

template <std::size_t I>
decltype(auto) get(const adl_tuple& value) {
  if constexpr (I == 0)
    return (value.first);
  else
    return (value.second);
}

struct member_tuple {
  int value;

  template <std::size_t I>
    requires(I == 0)
  int get() const {
    return value;
  }
};

/** @brief 单遍范围的 const 迭代仍会消费数据，诊断摘要不能遍历它。 */
struct reading_range {
  mutable int position{};

  struct iterator {
    using difference_type = std::ptrdiff_t;
    using value_type = std::optional<int>;
    using iterator_concept = std::input_iterator_tag;
    int* position;

    value_type operator*() const { return *position; }

    iterator& operator++() {
      ++*position;
      return *this;
    }

    void operator++(int) { ++*position; }

    bool operator==(std::default_sentinel_t) const { return *position >= 3; }
  };

  iterator begin() const { return {&position}; }

  std::default_sentinel_t end() const { return {}; }
};

/** @brief 诊断摘要不能领取结果或阻塞在同步 get()。 */
struct result_like {
  mutable int calls{};

  int get() const {
    ++calls;
    throw std::logic_error("get must not be called by formatting");
  }
};

/** @brief 向格式化输出迭代器写入时传播调用方异常。 */
struct failing_output {
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::output_iterator_tag;

  failing_output& operator*() { return *this; }

  failing_output& operator++() { return *this; }

  failing_output operator++(int) { return *this; }

  const failing_output& operator=(char) const { throw std::runtime_error("output failed"); }
};
}  // namespace stdio_format_test

namespace std {
template <>
struct tuple_size<stdio_format_test::adl_tuple> : integral_constant<size_t, 2> {};

template <size_t I>
struct tuple_element<I, stdio_format_test::adl_tuple> {
  using type = conditional_t<I == 0, int, optional<int>>;
};

template <>
struct tuple_size<stdio_format_test::member_tuple> : integral_constant<size_t, 1> {};

template <>
struct tuple_element<0, stdio_format_test::member_tuple> {
  using type = int;
};

template <>
struct formatter<stdio_format_test::mutable_value> final : formatter<int> {
  template <class Context>
  auto format(stdio_format_test::mutable_value& value, Context& context) const {
    return formatter<int>::format(value.number, context);
  }
};

template <>
struct formatter<stdio_format_test::native_value> : formatter<int> {
  template <class Context>
  auto format(const stdio_format_test::native_value& value, Context& context) const {
    return formatter<int>::format(value.value, context);
  }
};

template <>
struct formatter<stdio_format_test::throwing_value> : formatter<string_view> {
  template <class Context>
  auto format(const stdio_format_test::throwing_value&, Context&) const ->
      typename Context::iterator {
    throw runtime_error("format failed");
  }
};
}  // namespace std

namespace {
/** @brief 被格式化的惰性任务只有显式等待时才运行。 */
faio::task<int> lazy_value(int& starts) {
  ++starts;
  co_return 42;
}

/** @brief 每次只接受两个字节，验证 print_to 正确处理短写。 */
struct short_writer {
  std::string output;

  faio::task<faio::expected<std::size_t>> write(std::span<const char> bytes) {
    const auto count = std::min<std::size_t>(bytes.size(), 2);
    output.append(bytes.data(), count);
    co_return count;
  }
};

/** @brief 首批成功，第二批写入一个字节后报告错误。 */
struct failing_writer {
  std::string output;

  faio::task<faio::expected<std::size_t>> write(std::span<const char> bytes) {
    if (output.empty()) {
      const auto count = std::min<std::size_t>(3, bytes.size());
      output.append(bytes.data(), count);
      co_return count;
    }
    output.push_back(bytes.front());
    co_return std::unexpected{faio::error{EIO, 1}};
  }
};
}  // namespace

namespace stdio_format_test {
/** @brief 编译所有资源类型的诊断路径，不需要构造或启动资源。 */
template <class T>
void instantiate_diagnostic(const T& value) {
  (void)faio::stdio::format("{}", value);
  (void)faio::stdio::println("{}", value);
}

template void instantiate_diagnostic(const faio::error_context&);
template void instantiate_diagnostic(const faio::io::io_context&);
template void instantiate_diagnostic(const faio::io::readiness&);
template void instantiate_diagnostic(const faio::io::operation_options&);
template void instantiate_diagnostic(const faio::io::io_buffer&);
template void instantiate_diagnostic(const faio::io::io_transfer&);
template void instantiate_diagnostic(const faio::io::memory_stream&);
template void instantiate_diagnostic(const faio::net::host_port&);
template void instantiate_diagnostic(const faio::net::tcp_socket&);
template void instantiate_diagnostic(const faio::net::tcp_listener&);
template void instantiate_diagnostic(const faio::net::tcp_stream&);
template void instantiate_diagnostic(const faio::net::udp_socket&);
template void instantiate_diagnostic(const faio::net::datagram_message&);
template void instantiate_diagnostic(const faio::fs::file&);
template void instantiate_diagnostic(const faio::fs::file_metadata&);
template void instantiate_diagnostic(const faio::fs::permissions&);
template void instantiate_diagnostic(const faio::fs::directory_reader&);
template void instantiate_diagnostic(const faio::sync::mutex&);
template void instantiate_diagnostic(const faio::sync::semaphore&);
template void instantiate_diagnostic(const faio::sync::latch&);
template void instantiate_diagnostic(const faio::sync::condition_variable&);
template void instantiate_diagnostic(const faio::task<int>&);
template void instantiate_diagnostic(const faio::task_outcome<std::unique_ptr<int>>&);
template void instantiate_diagnostic(const faio::join_handle<int>&);
template void instantiate_diagnostic(const faio::shared_task<int>&);
template void instantiate_diagnostic(const faio::shared_result<int>&);
template void instantiate_diagnostic(const faio::config_builder&);
#if !defined(_WIN32)
template void instantiate_diagnostic(const faio::net::unix::unix_stream&);
template void instantiate_diagnostic(const faio::io::unix::async_fd<>&);
#endif
#if defined(FAIO_ENABLE_TLS) && FAIO_ENABLE_TLS
template void instantiate_diagnostic(const faio::tls::connection_info&);
template void instantiate_diagnostic(const faio::tls::client_config&);
template void instantiate_diagnostic(const faio::tls::tls_stream<faio::net::tcp_stream>&);
#endif
#if defined(FAIO_ENABLE_PROCESS) && FAIO_ENABLE_PROCESS
template void instantiate_diagnostic(const faio::process::output&);
template void instantiate_diagnostic(const faio::process::command&);
template void instantiate_diagnostic(const faio::process::child&);
#endif
}  // namespace stdio_format_test

TEST(StdioFormat, NativeFormattingPreservesSpecifiers) {
  EXPECT_EQ(faio::stdio::format("{:04x} {:.2f} {}", 15, 1.25, "hello"), "000f 1.25 hello");
  EXPECT_EQ(faio::stdio::format("{:04x}", stdio_format_test::native_value{42}), "002a");
  EXPECT_EQ(faio::stdio::format("{:>{}}", std::optional<int>{3}, 10), "   some(3)");
}

TEST(StdioFormat, OptionalExpectedAndVariantAreObservedWithoutConsumption) {
  const std::optional<int> empty;
  const std::expected<std::optional<int>, std::string> value{std::in_place, 42};
  const std::expected<int, std::string> failed{std::unexpect, "bad"};
  const std::expected<void, int> completed;
  EXPECT_EQ(faio::stdio::format("{} {} {} {}", empty, value, failed, completed),
            "none ok(some(42)) err(bad) ok()");
  const std::variant<int, std::optional<std::string>> variant{
      std::in_place_index<1>, std::in_place, "text"};
  EXPECT_EQ(faio::stdio::format("{}", variant), "variant(1, some(text))");
  const std::optional<std::unique_ptr<int>> move_only{std::in_place, std::make_unique<int>(7)};
  EXPECT_FALSE(faio::stdio::format("{}", move_only).empty());
  ASSERT_TRUE(move_only && *move_only);
  EXPECT_EQ(**move_only, 7);
}

TEST(StdioFormat, NestedRangesAndTuples) {
  const std::vector<std::optional<int>> range{{1}, {}, {3}};
  EXPECT_EQ(faio::stdio::format("{}", range), "[some(1), none, some(3)]");
  EXPECT_EQ(faio::stdio::format("{}", std::tuple{std::optional<int>{5}, 6}), "(some(5), 6)");
}

TEST(StdioFormat, NativeFormatterPreservesConstRequirements) {
  stdio_format_test::mutable_value value{7};
  EXPECT_EQ(faio::stdio::format("{}", value), "7");
  EXPECT_EQ(faio::stdio::format("{:04d}", value), "0007");
  const auto& immutable = value;
  EXPECT_NE(faio::stdio::format("{}", immutable).find("mutable_value"), std::string::npos);
  const std::optional<stdio_format_test::mutable_value> nested{value};
  EXPECT_NE(faio::stdio::format("{}", nested).find("some("), std::string::npos);
}

TEST(StdioFormat, CustomTupleAccess) {
  EXPECT_EQ(faio::stdio::format("{}", stdio_format_test::adl_tuple{1, 2}), "(1, some(2))");
  EXPECT_EQ(faio::stdio::format("{}", stdio_format_test::member_tuple{3}), "(3)");
}

TEST(StdioFormat, SinglePassRangesAreNotConsumed) {
  static_assert(std::ranges::input_range<const stdio_format_test::reading_range>);
  static_assert(!std::ranges::forward_range<const stdio_format_test::reading_range>);
  const stdio_format_test::reading_range value;
  EXPECT_NE(faio::stdio::format("{}", value).find("reading_range"), std::string::npos);
  EXPECT_EQ(value.position, 0);
}

TEST(StdioFormat, FaioValuesAndResourcesAreSafeToInspect) {
  const auto address = faio::net::socket_addr::parse("127.0.0.1:8080").value();
  EXPECT_EQ(faio::stdio::format("{}", address), "127.0.0.1:8080");
  EXPECT_NE(faio::stdio::format("{}", faio::error{EINVAL}).find("error"), std::string::npos);
  faio::io::io_buffer buffer{16};
  EXPECT_NE(faio::stdio::format("{}", buffer).find("size=0"), std::string::npos);
  int starts{};
  auto task = lazy_value(starts);
  EXPECT_NE(faio::stdio::format("{}", task).find("task"), std::string::npos);
  EXPECT_EQ(starts, 0);
  stdio_format_test::result_like result;
  EXPECT_FALSE(faio::stdio::format("{}", result).empty());
  EXPECT_EQ(result.calls, 0);
}

TEST(StdioFormat, PrintToOwnsFormattedArgumentsAndHandlesShortWrites) {
  faio::runtime::current_thread_runtime runtime;
  short_writer output;
  std::string text = "original";
  auto pending = faio::stdio::println_to(output, "{} {}", text, std::optional<int>{42});
  text = "changed";
  EXPECT_TRUE(runtime.block_on(std::move(pending)));
  EXPECT_EQ(output.output, "original some(42)\n");
  EXPECT_TRUE(runtime.block_on(faio::stdio::println_to(output)));
  EXPECT_EQ(output.output, "original some(42)\n\n");
}

TEST(StdioFormat, PrintToPreservesPartialErrorProgress) {
  faio::runtime::current_thread_runtime runtime;
  failing_writer output;
  const auto result = runtime.block_on(faio::stdio::print_to(output, "{}", "abcdef"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().value(), EIO);
  EXPECT_EQ(result.error().progress(), 4u);
  EXPECT_EQ(output.output, "abcd");
}

TEST(StdioFormat, FormattingErrorsAreSynchronous) {
  EXPECT_THROW((void)faio::stdio::print("{}", stdio_format_test::throwing_value{}),
               std::runtime_error);
}

TEST(StdioFormat, ExistingFaioFormattersPropagateOutputFailure) {
  auto context = std::make_shared<faio::error_context>();
  context->message.assign(4096, 'x');
  const auto error = faio::error{EINVAL}.with_context(context);
  EXPECT_THROW(std::format_to(stdio_format_test::failing_output{}, "{}", error),
               std::runtime_error);
  const auto config = faio::config_builder{}.build();
  EXPECT_THROW(std::format_to(stdio_format_test::failing_output{}, "{}", config),
               std::runtime_error);
}
