module;

#define FAIO_DETAIL_SEPARATE_DUPLEX 1
#include <faio/log/logger.hpp>

export module faio.log;
export import faio.common;

export namespace faio::log {
using ::faio::log::logger;
}  // namespace faio::log
