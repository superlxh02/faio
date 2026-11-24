module;

#define FAIO_DETAIL_SEPARATE_DUPLEX 1
#include <coroutine>
#include <tuple>
#include <faio/common/error.hpp>
#include <faio/detail/common/concepts.hpp>
#include <faio/detail/common/move_only_function.hpp>

export module faio.common;

// Coroutine syntax and structured bindings perform lookup in std at the
// importing call site. Keep their protocol names visible without a std module.
export namespace std {
using ::std::coroutine_handle;
using ::std::coroutine_traits;
using ::std::get;
using ::std::noop_coroutine;
using ::std::noop_coroutine_handle;
using ::std::noop_coroutine_promise;
using ::std::suspend_always;
using ::std::suspend_never;
using ::std::tuple_element;
using ::std::tuple_element_t;
using ::std::tuple_size;
using ::std::tuple_size_v;
}  // namespace std

export namespace faio {
using ::faio::Error;
using ::faio::constructible_to_char_slice;
using ::faio::error_context;
using ::faio::error_domain;
using ::faio::expected;
using ::faio::is_awaiter;
using ::faio::is_socket_address;
using ::faio::make_error;
using ::faio::move_only_function;
using ::faio::operation_cancelled;
}  // namespace faio
