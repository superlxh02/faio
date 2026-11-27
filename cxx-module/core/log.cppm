module;

#include <faio/log/logger.hpp>

export module faio.log;
export import faio.common;

// Inline logger allocation needs these overloads visible at the importing call site.
export using ::operator new;
export using ::operator new[];
export using ::operator delete;
export using ::operator delete[];

export namespace faio::log {
using ::faio::log::logger;
}  // namespace faio::log
