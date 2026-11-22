module;

#define FAIO_DETAIL_SEPARATE_DUPLEX 1
#include <faio/process/child.hpp>
#include <faio/process/command.hpp>

export module faio.process;
import faio;

export namespace faio::io {
using ::faio::io::OperationOptions;
}  // namespace faio::io

export namespace faio::process {
using ::faio::process::CaptureOptions;
using ::faio::process::Child;
using ::faio::process::ChildStderr;
using ::faio::process::ChildStdin;
using ::faio::process::ChildStdout;
using ::faio::process::Command;
using ::faio::process::EnvironmentChange;
using ::faio::process::EnvironmentVariable;
using ::faio::process::errc;
using ::faio::process::ExitStatus;
using ::faio::process::Output;
using ::faio::process::process_id;
using ::faio::process::Stdio;
using ::faio::process::StdioFileMode;
using ::faio::process::StdioKind;
}  // namespace faio::process
