module;

#include <faio/common/error.hpp>

export module faio.common;

export namespace faio {
using ::faio::Error;
using ::faio::error_context;
using ::faio::error_domain;
using ::faio::expected;
using ::faio::make_error;
using ::faio::operation_cancelled;
}  // namespace faio
