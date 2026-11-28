module;

#include <faio/runtime/default.hpp>
#include <faio/runtime/instance.hpp>
#include <faio/coroutine/awaiter.hpp>

export module faio.runtime;
export import faio.io;

export namespace faio {
using ::faio::anchored_runtime;
using ::faio::basic_runtime_anchor;
using ::faio::block_on;
using ::faio::config_builder;
using ::faio::on;
using ::faio::spawn;
using ::faio::spawn_blocking;
using ::faio::spawn_detached;
}  // namespace faio

export namespace faio::runtime {
using ::faio::runtime::basic_runtime;
using ::faio::runtime::blocking_pool_policy;
using ::faio::runtime::config;
using ::faio::runtime::configure;
using ::faio::runtime::current_thread_runtime;
using ::faio::runtime::mode;
using ::faio::runtime::multi_thread_runtime;
using ::faio::runtime::shutdown;
#if defined(__linux__)
using ::faio::runtime::io_backend;
#endif
}  // namespace faio::runtime

export namespace faio::experimental {
using ::faio::experimental::anchored_runtime;
using ::faio::experimental::basic_runtime_anchor;
using ::faio::experimental::on;
}  // namespace faio::experimental
