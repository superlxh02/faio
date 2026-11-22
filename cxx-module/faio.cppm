module;

#define FAIO_DETAIL_SEPARATE_DUPLEX 1
#include <faio/faio.hpp>
#if defined(_WIN32)
#include <faio/detail/io/platform/windows/operation_overloads.hpp>
#endif

export module faio;

// The public headers own the declarations and implementation. Keep the
// exported names here so changes to the public module API are easy to review.

export namespace faio {
using ::faio::ConfigBuilder;
using ::faio::Error;
using ::faio::anchored_runtime;
using ::faio::await_operation;
using ::faio::await_operation_for;
using ::faio::awaiter_facade;
using ::faio::basic_runtime_anchor;
using ::faio::basic_scheduler_ref;
using ::faio::basic_scope_context;
using ::faio::basic_task_group_context;
using ::faio::basic_task_lifetime_ref;
using ::faio::block_on;
using ::faio::branch_failure;
using ::faio::cleanup_error;
using ::faio::complete_before;
using ::faio::config_builder;
using ::faio::constructible_to_char_slice;
using ::faio::coroutine_awaiter;
using ::faio::coroutine_scheduler;
using ::faio::deadline_outcome;
using ::faio::deadline_status;
using ::faio::default_scheduler;
using ::faio::error_context;
using ::faio::error_domain;
using ::faio::expected;
using ::faio::for_each_concurrent;
using ::faio::group_child_result;
using ::faio::group_error_policy;
using ::faio::group_options;
using ::faio::group_result;
using ::faio::is_awaiter;
using ::faio::is_socket_address;
using ::faio::join;
using ::faio::join_all;
using ::faio::join_all_limited;
using ::faio::join_all_settled;
using ::faio::join_handle;
using ::faio::join_settled;
using ::faio::make_awaiter;
using ::faio::make_error;
using ::faio::make_error_code;
using ::faio::move_only_function;
using ::faio::on;
using ::faio::operation_cancelled;
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
using ::faio::spawn;
using ::faio::spawn_blocking;
using ::faio::spawn_detached;
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

export namespace faio::experimental {
using ::faio::experimental::anchored_runtime;
using ::faio::experimental::basic_runtime_anchor;
using ::faio::experimental::for_each_concurrent;
using ::faio::experimental::on;
using ::faio::experimental::serial_domain;
using ::faio::experimental::stream_producer;
using ::faio::experimental::stream_reader;
using ::faio::experimental::with_stream;
}  // namespace faio::experimental

export namespace faio::fs {
using ::faio::fs::DirBuilder;
using ::faio::fs::DirEntry;
using ::faio::fs::File;
using ::faio::fs::Metadata;
using ::faio::fs::OpenOptions;
using ::faio::fs::Permissions;
using ::faio::fs::ReadDir;
using ::faio::fs::canonicalize;
using ::faio::fs::copy;
using ::faio::fs::create_dir;
using ::faio::fs::create_dir_all;
using ::faio::fs::hard_link;
using ::faio::fs::metadata;
using ::faio::fs::path;
using ::faio::fs::read;
using ::faio::fs::read_dir;
using ::faio::fs::read_to_string;
using ::faio::fs::recursive_remove_options;
using ::faio::fs::remove_dir;
using ::faio::fs::remove_dir_all;
using ::faio::fs::remove_file;
using ::faio::fs::rename;
using ::faio::fs::seek_from;
using ::faio::fs::set_permissions;
using ::faio::fs::symlink;
using ::faio::fs::symlink_metadata;
using ::faio::fs::write;
}  // namespace faio::fs

export namespace faio::io {
using ::faio::io::BufReader;
using ::faio::io::BufStream;
using ::faio::io::BufWriter;
using ::faio::io::Chain;
using ::faio::io::DuplexStream;
using ::faio::io::Empty;
using ::faio::io::Interest;
using ::faio::io::Lines;
using ::faio::io::MemoryStream;
using ::faio::io::ReadHalf;
using ::faio::io::Ready;
using ::faio::io::Repeat;
using ::faio::io::ShutdownBehavior;
using ::faio::io::Sink;
using ::faio::io::SplitByDelimiter;
using ::faio::io::Take;
using ::faio::io::WriteHalf;
using ::faio::io::accept;
using ::faio::io::async_buf_reader;
using ::faio::io::async_reader;
using ::faio::io::async_seeker;
using ::faio::io::async_writer;
using ::faio::io::awaitable_result_t;
using ::faio::io::basic_io_driver_ref;
using ::faio::io::basic_io_submitter_ref;
using ::faio::io::borrowed_async_reader;
using ::faio::io::borrowed_async_writer;
using ::faio::io::borrowed_buffer;
using ::faio::io::borrowed_const_buffer;
using ::faio::io::cancel_reason;
using ::faio::io::chain;
using ::faio::io::close;
using ::faio::io::connect;
using ::faio::io::copy;
using ::faio::io::copy_bidirectional;
using ::faio::io::current_context;
using ::faio::io::drive_budget;
using ::faio::io::drive_result;
using ::faio::io::duplex;
using ::faio::io::empty;
using ::faio::io::engine_config;
using ::faio::io::io_awaitable_of;
using ::faio::io::io_buffer;
using ::faio::io::io_capabilities;
using ::faio::io::io_context;
using ::faio::io::io_driver_api;
using ::faio::io::io_driver_ref;
using ::faio::io::io_engine;
using ::faio::io::io_submitter_api;
using ::faio::io::io_submitter_ref;
using ::faio::io::io_transfer;
using ::faio::io::lines;
using ::faio::io::native_file_awaiter;
using ::faio::io::native_file_close;
using ::faio::io::native_file_close_awaiter;
using ::faio::io::native_file_op;
using ::faio::io::open;
using ::faio::io::openat;
using ::faio::io::operation_token;
using ::faio::io::operator|;
using ::faio::io::read;
using ::faio::io::read_buf;
using ::faio::io::read_exact;
using ::faio::io::read_line;
using ::faio::io::read_to_end;
using ::faio::io::read_to_string;
using ::faio::io::read_until;
using ::faio::io::ready;
using ::faio::io::recv;
using ::faio::io::recvmsg;
using ::faio::io::repeat;
using ::faio::io::send;
using ::faio::io::send_zc;
using ::faio::io::sendmsg;
using ::faio::io::sendmsg_zc;
using ::faio::io::shared_const_buffer;
using ::faio::io::shutdown;
using ::faio::io::shutdown_policy;
using ::faio::io::sink;
using ::faio::io::socket;
using ::faio::io::split;
using ::faio::io::split_by_delimiter;
using ::faio::io::supports_native;
using ::faio::io::take;
using ::faio::io::write;
using ::faio::io::write_all;
using ::faio::io::write_all_buf;
}  // namespace faio::io

#if !defined(_WIN32)
export namespace faio::io::unix {
using ::faio::io::unix::AsyncFd;
using ::faio::io::unix::AsyncFdReadyGuard;
using ::faio::io::unix::OwnedFd;
using ::faio::io::unix::native_fd_owner;
}  // namespace faio::io::unix
#endif

#if defined(_WIN32)
export namespace faio::io::windows {
using ::faio::io::windows::append_only_handle;
using ::faio::io::windows::as_socket;
using ::faio::io::windows::backend_box;
using ::faio::io::windows::encode_windows_error;
using ::faio::io::windows::encode_winsock_error;
using ::faio::io::windows::file_handle;
using ::faio::io::windows::file_transfer;
using ::faio::io::windows::format_windows_error;
using ::faio::io::windows::implicit_cursor_mutex;
using ::faio::io::windows::initialize_winsock;
using ::faio::io::windows::invalid_file_handle;
using ::faio::io::windows::invalid_socket_handle;
using ::faio::io::windows::iocp_backend;
using ::faio::io::windows::iocp_capabilities;
using ::faio::io::windows::make_io_error;
using ::faio::io::windows::make_iocp_backend;
using ::faio::io::windows::make_windows_error;
using ::faio::io::windows::make_winsock_error;
using ::faio::io::windows::native_error_mask;
using ::faio::io::windows::native_registration;
using ::faio::io::windows::open_file;
using ::faio::io::windows::overlapped_operation_state;
using ::faio::io::windows::owned_file_handle;
using ::faio::io::windows::owned_socket_handle;
using ::faio::io::windows::prepare_socket;
using ::faio::io::windows::query_file_position;
using ::faio::io::windows::recv_message;
using ::faio::io::windows::scalar_file;
using ::faio::io::windows::send_message;
using ::faio::io::windows::socket_extension;
using ::faio::io::windows::socket_failure;
using ::faio::io::windows::socket_handle;
using ::faio::io::windows::socket_vectors;
using ::faio::io::windows::submit_result;
using ::faio::io::windows::submit_status;
using ::faio::io::windows::update_file_position;
using ::faio::io::windows::vectored_file;
using ::faio::io::windows::win32_error_tag;
using ::faio::io::windows::winsock_errno;
using ::faio::io::windows::winsock_error_tag;
}  // namespace faio::io::windows
#endif

export namespace faio::log {
using ::faio::log::logger;
}  // namespace faio::log

export namespace faio::net {
using ::faio::net::DatagramMessage;
using ::faio::net::DatagramSend;
using ::faio::net::HostPort;
using ::faio::net::Ipv4Addr;
using ::faio::net::Ipv6Addr;
using ::faio::net::OwnedDatagram;
using ::faio::net::SocketAddr;
using ::faio::net::TcpListener;
using ::faio::net::TcpSocket;
using ::faio::net::TcpStream;
using ::faio::net::UdpDatagram;
using ::faio::net::UdpSocket;
using ::faio::net::accept_options;
using ::faio::net::accept_placement;
using ::faio::net::address;
using ::faio::net::lookup_host;
using ::faio::net::owned_native_socket;
using ::faio::net::v4addr;
using ::faio::net::v6addr;
}  // namespace faio::net

#if !defined(_WIN32)
export namespace faio::net::unix {
using ::faio::net::unix::PeerCredentials;
using ::faio::net::unix::UnixAddr;
using ::faio::net::unix::UnixDatagram;
using ::faio::net::unix::UnixListener;
using ::faio::net::unix::UnixSocket;
using ::faio::net::unix::UnixStream;
using ::faio::net::unix::address;
}  // namespace faio::net::unix
#endif

#if !defined(_WIN32)
export namespace faio::net::unix::pipe {
using ::faio::net::unix::pipe::Receiver;
using ::faio::net::unix::pipe::Sender;
using ::faio::net::unix::pipe::owned_native_fd;
using ::faio::net::unix::pipe::pair;
}  // namespace faio::net::unix::pipe
#endif

export namespace faio::runtime {
using ::faio::runtime::Config;
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

export namespace faio::sync {
using ::faio::sync::barrier;
using ::faio::sync::basic_barrier;
using ::faio::sync::basic_condition_variable;
using ::faio::sync::basic_latch;
using ::faio::sync::basic_mutex;
using ::faio::sync::basic_semaphore;
using ::faio::sync::condition_variable;
using ::faio::sync::latch;
using ::faio::sync::mpsc;
using ::faio::sync::mutex;
using ::faio::sync::semaphore;
}  // namespace faio::sync

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

export namespace faio::time {
using ::faio::time::MissedTickBehavior;
using ::faio::time::interval;
using ::faio::time::interval_at;
using ::faio::time::sleep;
using ::faio::time::sleep_until;
using ::faio::time::timeout;
using ::faio::time::timeout_at;
}  // namespace faio::time

export namespace faio::util {
using ::faio::util::cpu_relax;
using ::faio::util::static_log;
using ::faio::util::static_pow;
}  // namespace faio::util
