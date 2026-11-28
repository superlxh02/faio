module;

#include <faio/common/error.hpp>
#include <faio/common/match.hpp>

export module faio.common;

export namespace faio {
using ::faio::error;
using ::faio::error_context;
using ::faio::error_domain;
using ::faio::expected;
using ::faio::make_error;
using ::faio::match;
using ::faio::ok;
using ::faio::err;
using ::faio::some;
using ::faio::none;
using ::faio::case_;
using ::faio::when;
using ::faio::otherwise;
using ::faio::type_case;
using ::faio::index_case;
using ::faio::operation_cancelled;
}  // namespace faio
