/** @file @brief time/time.hpp 独立首先包含，再引入具体操作验证两种策略。 */
#include "faio/detail/io/awaiter/read.hpp"
#include "faio/detail/io/awaiter/recv.hpp"
#include "faio/detail/io/awaiter/send.hpp"
#include "faio/detail/time/time.hpp"

namespace compact_time_header_test {
using faio::io::detail::Read;
using faio::io::detail::Recv;
using faio::io::detail::Send;
static_assert(std::same_as<decltype(faio::time::timeout(std::declval<Recv&&>(),
                                                        std::chrono::milliseconds{1})),
                           Recv>);
static_assert(std::same_as<decltype(faio::time::timeout_at(
                               std::declval<Send&>(), std::chrono::steady_clock::time_point{})),
                           Send&>);
static_assert(std::same_as<decltype(faio::time::timeout(std::declval<Read&&>(),
                                                        std::chrono::milliseconds{1})),
                           Read>);
static_assert(std::same_as<decltype(faio::time::timeout_at(
                               std::declval<Read&>(), std::chrono::steady_clock::time_point{})),
                           Read&>);
template <class T>
concept timeout_configurable = requires(T&& operation) {
  faio::time::timeout(std::forward<T>(operation), std::chrono::milliseconds{1});
};

struct counterfeit {
  using io_registrant_base_type = counterfeit;
};

static_assert(!timeout_configurable<const Recv&>);
static_assert(!timeout_configurable<counterfeit>);
static_assert(!timeout_configurable<faio::time::detail::Sleep>);
}  // namespace compact_time_header_test
