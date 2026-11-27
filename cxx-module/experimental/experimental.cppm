module;

#include <faio/experimental/runtime.h>
#include <faio/experimental/runtime_options.h>

#if FAIO_EXPERIMENTAL_HAS_REFLECTION
#include <faio/experimental/reflection.h>
#include <faio/detail/experimental/async_entry.hpp>
#endif
#if FAIO_EXPERIMENTAL_TASK_LIFECYCLE
#include <faio/experimental/lifecycle.h>
#endif
#if FAIO_EXPERIMENTAL_HAS_EXECUTION
#include <faio/experimental/execution.h>
#endif

export module faio.experimental;
export import faio;

export namespace faio::experimental {
using ::faio::experimental::current_thread_defaults;
using ::faio::experimental::current_thread_runtime;
using ::faio::experimental::current_thread_scheduler;
using ::faio::experimental::entry_exit_policy;
using ::faio::experimental::execution_scheduler;
using ::faio::experimental::io_backend_option;
using ::faio::experimental::multi_thread_defaults;
using ::faio::experimental::multi_thread_runtime;
using ::faio::experimental::multi_thread_scheduler;
using ::faio::experimental::options_error;
using ::faio::experimental::runtime_options;
using ::faio::experimental::runtime_ref;
using ::faio::experimental::sender_join_handle;
using ::faio::experimental::this_runtime;
using ::faio::experimental::to_runtime_config;
using ::faio::experimental::validate_options;

#if FAIO_EXPERIMENTAL_HAS_REFLECTION
using ::faio::experimental::main;
using ::faio::experimental::main_annotation_type;
using ::faio::experimental::runtime_options_of;

namespace detail {
using ::faio::experimental::detail::entry_function_of;
using ::faio::experimental::detail::run_main;
}  // namespace detail
#endif
#if FAIO_EXPERIMENTAL_TASK_LIFECYCLE
using ::faio::experimental::lifecycle;
#endif
#if FAIO_EXPERIMENTAL_HAS_EXECUTION
using ::faio::experimental::as_sender;
using ::faio::experimental::execution_error;
using ::faio::experimental::schedule_sender;
using ::faio::experimental::spawn_sender;
using ::faio::experimental::with_default_runtime;
#endif
}  // namespace faio::experimental
