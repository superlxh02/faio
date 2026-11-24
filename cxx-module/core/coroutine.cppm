module;

#define FAIO_DETAIL_SEPARATE_DUPLEX 1
#include <faio/coroutine/awaiter.hpp>
#include <faio/coroutine/composition.hpp>
#include <faio/coroutine/lifecycle.hpp>
#include <faio/coroutine/sequence.hpp>
#include <faio/coroutine/task.hpp>

export module faio.coroutine;
export import faio.common;

export namespace faio {
using ::faio::await_operation;
using ::faio::await_operation_for;
using ::faio::awaiter_facade;
using ::faio::basic_scheduler_ref;
using ::faio::basic_scope_context;
using ::faio::basic_task_group_context;
using ::faio::basic_task_lifetime_ref;
using ::faio::branch_failure;
using ::faio::cleanup_error;
using ::faio::complete_before;
using ::faio::coroutine_awaiter;
using ::faio::coroutine_scheduler;
using ::faio::deadline_outcome;
using ::faio::deadline_status;
using ::faio::default_scheduler;
using ::faio::for_each_concurrent;
using ::faio::group_child_result;
using ::faio::group_error_policy;
using ::faio::group_options;
using ::faio::group_result;
using ::faio::join;
using ::faio::join_all;
using ::faio::join_all_limited;
using ::faio::join_all_settled;
using ::faio::join_handle;
using ::faio::join_settled;
using ::faio::make_awaiter;
using ::faio::make_error_code;
using ::faio::race_in;
using ::faio::race_result;
using ::faio::ready_sink;
using ::faio::same_scheduler;
using ::faio::scheduler_api;
using ::faio::scheduler_ref;
using ::faio::scheduler_ref_for;
using ::faio::scope;
using ::faio::scope_context;
using ::faio::scoped_join_handle;
using ::faio::select;
using ::faio::select_result;
using ::faio::select_success;
using ::faio::select_success_result;
using ::faio::serial_domain;
using ::faio::settle;
using ::faio::shared_result;
using ::faio::shared_task;
using ::faio::stream_producer;
using ::faio::stream_reader;
using ::faio::task;
using ::faio::task_exit;
using ::faio::task_group;
using ::faio::task_group_context;
using ::faio::task_group_errc;
using ::faio::task_group_error_category;
using ::faio::task_lifecycle_policy;
using ::faio::task_lifetime;
using ::faio::task_lifetime_api;
using ::faio::task_lifetime_ref;
using ::faio::task_outcome;
using ::faio::task_outcome_status;
using ::faio::task_stop_reason;
using ::faio::with_cleanup;
using ::faio::with_stop_token;
using ::faio::with_stream;
}  // namespace faio

export namespace faio::experimental {
using ::faio::experimental::for_each_concurrent;
using ::faio::experimental::serial_domain;
using ::faio::experimental::stream_producer;
using ::faio::experimental::stream_reader;
using ::faio::experimental::with_stream;
}  // namespace faio::experimental

export namespace faio::this_coro {
using ::faio::this_coro::scheduler;
using ::faio::this_coro::scheduler_t;
using ::faio::this_coro::stop_token;
using ::faio::this_coro::stop_token_t;
using ::faio::this_coro::worker_id;
using ::faio::this_coro::worker_id_t;
using ::faio::this_coro::yield;
using ::faio::this_coro::yield_if_needed;
using ::faio::this_coro::yield_if_needed_t;
using ::faio::this_coro::yield_t;
}  // namespace faio::this_coro
