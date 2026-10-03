/** @file @brief timeout.hpp 独立首先包含；不能依赖 faio.hpp 建立模板声明。 */
#include "faio/detail/time/timeout.hpp"
namespace compact_timeout_header_test {
class complete_operation
    : public faio::io::detail::IORegistrantAwaiter<complete_operation> {
public:
  using Base = faio::io::detail::IORegistrantAwaiter<complete_operation>;
  using Base::Base;
};
static_assert(faio::io::detail::io_registrant_operation<complete_operation>);
static_assert(faio::io::detail::io_registrant_operation<
              faio::time::detail::Timeout<complete_operation>>);
static_assert(
    std::is_constructible_v<faio::time::detail::Timeout<complete_operation>,
                            complete_operation &&>);
} // namespace compact_timeout_header_test
