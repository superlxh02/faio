/** @file @brief 公共 detail/time.hpp 独立首先包含，默认模板参数只有一份定义。 */
#include "faio/detail/time.hpp"
#include "faio/detail/io/awaiter/recv.hpp"
#include "faio/detail/io/awaiter/send.hpp"
namespace compact_public_time_header_test {
using Recv = faio::io::detail::Recv;
using Send = faio::io::detail::Send;
using WrappedRecv = faio::time::detail::Timeout<Recv>;
using WrappedSend = faio::time::detail::Timeout<Send>;
static_assert(faio::io::detail::io_registrant_operation<WrappedRecv>);
static_assert(faio::io::detail::io_registrant_operation<WrappedSend>);
static_assert(std::is_move_constructible_v<WrappedRecv>);
static_assert(!std::is_copy_constructible_v<WrappedSend>);
} // namespace compact_public_time_header_test
