module;

#include <faio/io/native.hpp>
#include <faio/io/stream.hpp>
#include <faio/detail/io.hpp>
#include <faio/detail/io/engine.hpp>

#if defined(_WIN32)
#include <faio/detail/io/platform/windows/operation_overloads.hpp>
#endif

export module faio.io;
export import faio.coroutine;

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
using ::faio::io::OperationOptions;
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
