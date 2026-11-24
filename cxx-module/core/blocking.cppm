module;

#define FAIO_DETAIL_SEPARATE_DUPLEX 1
#include <faio/detail/blocking/call.hpp>
#include <faio/detail/blocking/service.hpp>

export module faio.blocking;
export import faio.coroutine;

export namespace faio::blocking {
using ::faio::blocking::basic_service_ref;
using ::faio::blocking::call;
using ::faio::blocking::call_awaiter;
using ::faio::blocking::call_cleanup;
using ::faio::blocking::call_on;
using ::faio::blocking::service;
using ::faio::blocking::service_api;
using ::faio::blocking::service_ref;
using ::faio::blocking::startup_policy;
}  // namespace faio::blocking
