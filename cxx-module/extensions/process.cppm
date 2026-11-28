module;

#include <faio/process/child.hpp>
#include <faio/process/command.hpp>

export module faio.process;
export import faio.fs;
export import faio.runtime;

export namespace faio::process {
using ::faio::process::capture_options;
using ::faio::process::child;
using ::faio::process::child_stderr;
using ::faio::process::child_stdin;
using ::faio::process::child_stdout;
using ::faio::process::command;
using ::faio::process::environment_change;
using ::faio::process::environment_variable;
using ::faio::process::errc;
using ::faio::process::exit_status;
using ::faio::process::output;
using ::faio::process::process_id;
using ::faio::process::stdio;
using ::faio::process::stdio_file_mode;
using ::faio::process::stdio_kind;
}  // namespace faio::process
