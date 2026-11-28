module;

#include <faio/stdio/print.hpp>
#include <faio/stdio/stream.hpp>

export module faio.stdio;
export import faio.io;
export import faio.runtime;

export namespace faio::stdio {
using ::faio::stdio::stdin_stream;
using ::faio::stdio::stdout_stream;
using ::faio::stdio::stderr_stream;
using ::faio::stdio::in;
using ::faio::stdio::out;
using ::faio::stdio::err;
using ::faio::stdio::standard_input;
using ::faio::stdio::standard_output;
using ::faio::stdio::standard_error;
using ::faio::stdio::format_string;
using ::faio::stdio::format;
using ::faio::stdio::print;
using ::faio::stdio::println;
using ::faio::stdio::eprint;
using ::faio::stdio::eprintln;
using ::faio::stdio::print_to;
using ::faio::stdio::println_to;
}  // namespace faio::stdio
