#include <faio/detail/io/protocol/backend.hpp>
#include <faio/detail/io/protocol/reactor.hpp>
#include <faio/detail/io/traits/backend.hpp>
#include <faio/detail/io/traits/reactor.hpp>
#include <faio/detail/io/base/backend.hpp>
#include <faio/detail/io/base/reactor.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

namespace {
using namespace faio::io::detail;

struct incomplete_backend;
struct incomplete_reactor;
// 未选择具体平台时也能声明静态拥有者；类型检查留在实际构造点。
static_assert(sizeof(basic_backend_box<incomplete_backend>)
              == sizeof(std::unique_ptr<incomplete_backend>));
static_assert(sizeof(basic_reactor_box<incomplete_reactor>)
              == sizeof(std::unique_ptr<incomplete_reactor>));
static_assert(!std::is_polymorphic_v<basic_backend_box<incomplete_backend>>);
static_assert(!std::is_polymorphic_v<basic_reactor_box<incomplete_reactor>>);
static_assert(std::is_trivially_copyable_v<backend_event>);
static_assert(std::is_trivially_copyable_v<readiness_event>);
static_assert(std::is_same_v<decltype(native_registration{}.value), std::uintptr_t>);
static_assert(sizeof(backend_operation{}.token) == sizeof(std::uint64_t));
}  // namespace

bool module_protocol_header_smoke() {
  constexpr auto descriptor = std::numeric_limits<std::uintptr_t>::max() - 7;
  constexpr std::uint64_t token = 0xfedcba9876543210ULL;
  const faio::io::detail::native_registration handle{
      descriptor, faio::io::detail::native_handle_kind::windows_handle};
  const faio::io::detail::backend_event event{
      faio::io::detail::backend_event_kind::result, token, 41, 0};
  const auto copied = event;
  return handle.value == descriptor && copied.key == token && copied.result == 41;
}
